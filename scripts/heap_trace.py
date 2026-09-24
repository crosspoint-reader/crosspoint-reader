#!/usr/bin/env python3
"""Decode and analyze the heap trace stream of the `heaptrace` firmware env.

The firmware (src/util/HeapTrace.cpp) prints `@HT <seq> <ms> <base64> <fletcher16>`
lines. This tool replays them into a live-allocation map, reconciles the map with
the heap walks in each snapshot, and attributes allocations to call sites via
addr2line. See docs/heap-trace.md.

Input is a monitor `events.jsonl`, a raw `serial.bin`, or any text capture.
"""

from __future__ import annotations

import argparse
import base64
import bisect
import collections
import html
import json
import re
import shutil
import statistics
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

LINE_RE = re.compile(r"@HT (\d+) (\d+) ([A-Za-z0-9+/=]+) ([0-9a-f]{4})")
USED_BIT = 0x80000000
# Frames that describe the allocator rather than the code that asked for memory.
ALLOCATOR_FRAME_RE = re.compile(
    r"^(malloc|calloc|realloc|free|_malloc_r|_calloc_r|_realloc_r|heap_caps_|multi_heap|tlsf_|"
    r"operator new|operator delete|esp_heap_trace|std::__new_allocator|__gnu_cxx::new_allocator|"
    r"std::allocator|std::allocator_traits|std::_Vector_base|std::vector<.*>::_M_|"
    r"std::__cxx11::basic_string<.*>::_M_|std::basic_string<.*>::_M_|std::__detail::_Hashtable_alloc|"
    r"makeUniqueNoThrow|pvPortMalloc|vPortFree|pvalloc|strdup|__strdup|_strdup_r|"
    r".*freeink::font::psramNew|freeink_font_alloc|HalMemory::allocate|"
    r".* makeUniqueNoThrow<|.*std::make_unique<|std::__detail::_MakeUniq)"
)


def fletcher16(data: bytes) -> int:
    sum1 = sum2 = 0
    for byte in data:
        sum1 = (sum1 + byte) % 255
        sum2 = (sum2 + sum1) % 255
    return (sum2 << 8) | sum1


def read_lines(path: Path):
    """Yields (host_time or None, text) for every log line in the capture."""
    if path.suffix == ".jsonl":
        with path.open(encoding="utf-8", errors="replace") as handle:
            for raw in handle:
                try:
                    event = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                if event.get("type") == "log" and "line" in event:
                    yield event.get("time"), event["line"]
        return
    data = path.read_bytes().decode("utf-8", errors="replace")
    for text in data.splitlines():
        yield None, text


@dataclass
class Alloc:
    ptr: int
    size: int
    task: int
    callers: tuple
    ms: int
    unknown: bool = False  # Found by a heap walk without a matching trace record.


@dataclass
class Snapshot:
    snap_id: int
    ms: int
    heaps: list = field(default_factory=list)  # (start, end)
    blocks: list = field(default_factory=list)  # (block_ptr, size, used)
    stats: dict = field(default_factory=dict)
    verified: int = 0
    added_unknown: int = 0
    removed_stale: int = 0
    complete: bool = False
    owners: dict = field(default_factory=dict)  # block_ptr -> Alloc


class LiveMap:
    """Live allocations keyed by user pointer, kept sorted for range queries."""

    def __init__(self):
        self.starts: list[int] = []
        self.allocs: dict[int, Alloc] = {}

    def _remove(self, ptr: int):
        index = bisect.bisect_left(self.starts, ptr)
        del self.starts[index]
        del self.allocs[ptr]

    def in_range(self, start: int, end: int):
        # Allocations whose [ptr, ptr+size) intersects [start, end).
        index = bisect.bisect_left(self.starts, start)
        if index > 0:
            previous = self.allocs[self.starts[index - 1]]
            if previous.ptr + max(previous.size, 1) > start:
                index -= 1
        found = []
        while index < len(self.starts) and self.starts[index] < end:
            found.append(self.allocs[self.starts[index]])
            index += 1
        return found

    def add(self, alloc: Alloc) -> int:
        # An overlapping live entry must have been freed without a record
        # (a dropped record, or an in-place realloc that reports no free).
        evicted = 0
        for stale in self.in_range(alloc.ptr, alloc.ptr + max(alloc.size, 1)):
            self._remove(stale.ptr)
            evicted += 1
        bisect.insort(self.starts, alloc.ptr)
        self.allocs[alloc.ptr] = alloc
        return evicted

    def free(self, ptr: int) -> bool:
        if ptr in self.allocs:
            self._remove(ptr)
            return True
        return False


class Replay:
    def __init__(self):
        self.live = LiveMap()
        self.tasks: dict[int, str] = {}
        self.snapshots: list[Snapshot] = []
        self.events: list[tuple] = []  # ('A', ms, alloc) / ('F', ms, ptr, alloc_or_None)
        self.lines = 0
        self.bad_lines = 0
        self.seq_gaps = 0
        self.dropped = 0
        self.evicted = 0
        self.unmatched_frees = 0
        self.expected_seq = None
        self.current: Snapshot | None = None
        self.pass_range = None
        self.pass_blocks: list = []
        self.boots = 0

    def feed_line(self, text: str):
        for match in LINE_RE.finditer(text):
            seq, ms, payload, check = int(match[1]), int(match[2]), match[3], int(match[4], 16)
            try:
                raw = base64.b64decode(payload, validate=True)
            except ValueError:
                self.bad_lines += 1
                continue
            if fletcher16(raw) != check:
                self.bad_lines += 1
                continue
            if self.expected_seq is not None and (seq == 0 or seq < self.expected_seq):
                self._reboot()
            elif self.expected_seq is not None and seq != self.expected_seq:
                self.seq_gaps += 1
            self.expected_seq = seq + 1
            self.lines += 1
            self._records(raw, ms)

    def _reboot(self):
        self.boots += 1
        self.live = LiveMap()
        self.current = None
        self.pass_range = None

    def _records(self, raw: bytes, ms: int):
        offset = 0
        while offset < len(raw):
            kind = chr(raw[offset])
            if kind == "A":
                ptr, size, task, count = struct.unpack_from("<IIIB", raw, offset + 1)
                callers = struct.unpack_from(f"<{count}I", raw, offset + 14)
                offset += 14 + 4 * count
                alloc = Alloc(ptr, size, task, callers, ms)
                self.evicted += self.live.add(alloc)
                self.events.append(("A", ms, alloc))
            elif kind == "F":
                (ptr,) = struct.unpack_from("<I", raw, offset + 1)
                offset += 5
                owner = self.live.allocs.get(ptr)
                if owner is None:
                    # A walk-discovered block is keyed by its block pointer, which
                    # precedes the user pointer by the poisoning header.
                    containing = [a for a in self.live.in_range(ptr, ptr + 1) if a.unknown]
                    owner = containing[0] if containing else None
                if owner is None or not self.live.free(owner.ptr):
                    self.unmatched_frees += 1
                self.events.append(("F", ms, ptr, owner))
            elif kind == "D":
                (self.dropped,) = struct.unpack_from("<I", raw, offset + 1)
                offset += 5
            elif kind == "S":
                snap_id, _version, _depth = struct.unpack_from("<IBB", raw, offset + 1)
                offset += 7
                self.current = Snapshot(snap_id, ms)
            elif kind == "N":
                task, length = struct.unpack_from("<IB", raw, offset + 1)
                self.tasks[task] = raw[offset + 6 : offset + 6 + length].decode("ascii", "replace")
                offset += 6 + length
            elif kind == "H":
                start, end = struct.unpack_from("<II", raw, offset + 1)
                offset += 9
                if self.current:
                    self.current.heaps.append((start, end))
            elif kind == "P":
                _snap, heap_start, start = struct.unpack_from("<III", raw, offset + 1)
                offset += 13
                self.pass_range = (heap_start, start)
                self.pass_blocks = []
            elif kind == "B":
                ptr, size_used = struct.unpack_from("<II", raw, offset + 1)
                offset += 9
                self.pass_blocks.append((ptr, size_used & ~USED_BIT, bool(size_used & USED_BIT)))
            elif kind == "Q":
                end, _count = struct.unpack_from("<II", raw, offset + 1)
                offset += 9
                if self.current and self.pass_range:
                    self._reconcile(self.pass_range[1], end, self.pass_blocks)
                self.pass_range = None
            elif kind == "E":
                values = struct.unpack_from("<IIIIII", raw, offset + 1)
                offset += 25
                if self.current:
                    keys = ("snap", "free", "largest", "min_free", "allocated_blocks", "free_blocks")
                    self.current.stats = dict(zip(keys, values))
                    self.current.complete = True
                    self.snapshots.append(self.current)
                    self.current = None
            elif kind == "X":
                offset += 5
                self.current = None
            else:
                self.bad_lines += 1
                return

    def _reconcile(self, start: int, end: int, blocks):
        snap = self.current
        covered = set()
        for ptr, size, used in blocks:
            snap.blocks.append((ptr, size, used))
            inside = [a for a in self.live.in_range(ptr, ptr + size) if ptr <= a.ptr < ptr + size]
            if not used:
                continue
            if inside:
                keep = inside[0]
                for extra in inside[1:]:
                    self.live.free(extra.ptr)
                    snap.removed_stale += 1
                snap.verified += 0 if keep.unknown else 1
            else:
                keep = Alloc(ptr, size, 0, (), snap.ms, unknown=True)
                self.live.add(keep)
                snap.added_unknown += 1
            covered.add(keep.ptr)
            snap.owners[ptr] = keep
        for alloc in self.live.in_range(start, end):
            if alloc.ptr not in covered and start <= alloc.ptr < end:
                self.live.free(alloc.ptr)
                snap.removed_stale += 1


class Symbolizer:
    def __init__(self, elf: Path | None):
        self.elf = elf if elf and elf.exists() else None
        self.cache: dict[int, str] = {}
        self.tool = None
        if self.elf:
            candidates = list(Path.home().glob(".platformio/packages/toolchain-riscv32-esp/bin/*-addr2line"))
            self.tool = str(candidates[0]) if candidates else shutil.which("riscv32-esp-elf-addr2line")

    def resolve(self, addresses):
        missing = sorted({a for a in addresses if a not in self.cache})
        if not missing:
            return
        if not self.tool:
            for address in missing:
                self.cache[address] = f"0x{address:08x}"
            return
        for index in range(0, len(missing), 400):
            chunk = missing[index : index + 400]
            # A return address points after the call; step back into it.
            args = [self.tool, "-f", "-C", "-e", str(self.elf)] + [hex(a - 2) for a in chunk]
            output = subprocess.run(args, capture_output=True, text=True, check=False).stdout.splitlines()
            for position, address in enumerate(chunk):
                function = output[2 * position] if 2 * position < len(output) else "??"
                location = output[2 * position + 1] if 2 * position + 1 < len(output) else "??:0"
                location = re.sub(r".*/include/c\+\+/[^/]+/(bits/)?", "c++/", location)
                location = re.sub(r".*/framework-arduinoespressif32/", "arduino/", location)
                location = re.sub(r".*/(src|lib|freeink-sdk)/", r"\1/", location)
                location = re.sub(r" \(discriminator \d+\)", "", location)
                self.cache[address] = f"{function} ({location})"

    def site(self, alloc: Alloc, frames: int = 1) -> str:
        if alloc.unknown:
            return "<untraced: allocated before trace or record lost>"
        if not alloc.callers:
            return "<no frame-pointer caller>"
        names = [self.cache.get(c, f"0x{c:08x}") for c in alloc.callers]
        user = [n for n in names if not ALLOCATOR_FRAME_RE.match(n)]
        chosen = (user or names)[:frames]
        return " <- ".join(chosen)


def load(args) -> tuple[Replay, Symbolizer]:
    replay = Replay()
    for _time, text in read_lines(Path(args.capture)):
        replay.feed_line(text)
    symbolizer = Symbolizer(Path(args.elf) if args.elf else None)
    addresses = set()
    for event in replay.events:
        if event[0] == "A":
            addresses.update(event[2].callers)
    for alloc in replay.live.allocs.values():
        addresses.update(alloc.callers)
    symbolizer.resolve(addresses)
    return replay, symbolizer


def kib(value: int) -> str:
    return f"{value / 1024:.1f}K"


def free_histogram(blocks):
    edges = [(256, "<256"), (1024, "<1K"), (4096, "<4K"), (16384, "<16K"), (1 << 32, ">=16K")]
    counts = collections.OrderedDict((label, [0, 0]) for _, label in edges)
    for _ptr, size, used in blocks:
        if used:
            continue
        for edge, label in edges:
            if size < edge:
                counts[label][0] += 1
                counts[label][1] += size
                break
    return counts


def pick_snapshot(replay: Replay, which: str | None) -> Snapshot:
    if not replay.snapshots:
        sys.exit("No complete snapshot in the capture; send CMD:HEAPTRACE SNAP")
    if which is None or which == "last":
        return replay.snapshots[-1]
    for snap in replay.snapshots:
        if snap.snap_id == int(which):
            return snap
    sys.exit(f"Snapshot {which} not found")


def cmd_summary(args):
    replay, sym = load(args)
    allocs = sum(1 for e in replay.events if e[0] == "A")
    frees = sum(1 for e in replay.events if e[0] == "F")
    print(f"lines={replay.lines} bad={replay.bad_lines} seq_gaps={replay.seq_gaps} dropped_records={replay.dropped} "
          f"boots={replay.boots + 1 if replay.lines else 0}")
    print(f"allocs={allocs} frees={frees} unmatched_frees={replay.unmatched_frees} overlap_evictions={replay.evicted} "
          f"snapshots={len(replay.snapshots)}")
    if replay.dropped or replay.seq_gaps or replay.bad_lines:
        print("WARNING: records were lost; per-site counts are lower bounds and live data is exact only at snapshots")
    by_site = collections.defaultdict(lambda: [0, 0, []])
    for event in replay.events:
        if event[0] == "A":
            entry = by_site[sym.site(event[2], args.frames)]
            entry[0] += 1
            entry[1] += event[2].size
            entry[2].append(event[2].size)
    print(f"\nTop allocation sites by count ({args.top}):")
    print(f"{'count':>8} {'bytes':>10} {'median':>7}  site")
    for site, (count, total, sizes) in sorted(by_site.items(), key=lambda kv: -kv[1][0])[: args.top]:
        print(f"{count:8d} {total:10d} {int(statistics.median(sizes)):7d}  {site}")
    print_live(replay.live.allocs.values(), sym, args, "Live at end of capture")


def print_live(allocs, sym, args, title):
    by_site = collections.defaultdict(lambda: [0, 0])
    for alloc in allocs:
        entry = by_site[sym.site(alloc, args.frames)]
        entry[0] += 1
        entry[1] += alloc.size
    total = sum(v[1] for v in by_site.values())
    print(f"\n{title}: {sum(v[0] for v in by_site.values())} blocks, {total} bytes")
    print(f"{'blocks':>7} {'bytes':>9}  site")
    for site, (count, size) in sorted(by_site.items(), key=lambda kv: -kv[1][1])[: args.top]:
        print(f"{count:7d} {size:9d}  {site}")


def cmd_snapshots(args):
    replay, _sym = load(args)
    print(f"{'snap':>4} {'ms':>9} {'free':>8} {'largest':>8} {'frag':>5} {'freeblk':>7} {'usedblk':>7} "
          f"{'verified':>8} {'untraced':>8} {'stale':>5}  free-block histogram (count/bytes)")
    for snap in replay.snapshots:
        stats = snap.stats
        frag = 1 - stats["largest"] / stats["free"] if stats["free"] else 0
        hist = " ".join(f"{label}:{c}/{kib(b)}" for label, (c, b) in free_histogram(snap.blocks).items())
        print(f"{snap.snap_id:4d} {snap.ms:9d} {stats['free']:8d} {stats['largest']:8d} {frag:5.2f} "
              f"{stats['free_blocks']:7d} {stats['allocated_blocks']:7d} {snap.verified:8d} {snap.added_unknown:8d} "
              f"{snap.removed_stale:5d}  {hist}")


def cmd_frag(args):
    replay, sym = load(args)
    snap = pick_snapshot(replay, args.snap)
    stats = snap.stats
    print(f"snapshot {snap.snap_id} at {snap.ms} ms: free={stats['free']} largest={stats['largest']} "
          f"free_blocks={stats['free_blocks']} used_blocks={stats['allocated_blocks']}")
    for label, (count, total) in free_histogram(snap.blocks).items():
        print(f"  free {label:>6}: {count:5d} blocks {total:8d} bytes")
    ordered = sorted(snap.blocks)
    free_blocks = sorted((b for b in ordered if not b[2]), key=lambda b: -b[1])
    print("\nLargest free blocks:")
    for ptr, size, _used in free_blocks[: args.top]:
        print(f"  0x{ptr:08x} {size:7d}")

    runs = pinning_runs(snap, args.max_run)
    print(f"\nPinning runs (<= {args.max_run} used blocks between two free blocks), largest merged region first; "
          f"current largest free = {stats['largest']}")
    print(f"{'merged':>7} {'bytes':>6} {'age_ms':>7} {'task':<15} site")
    for merged, run in runs[: args.top]:
        flag = "*" if merged > stats["largest"] else " "
        for position, (ptr, size, _used) in enumerate(run):
            owner = snap.owners.get(ptr)
            age = snap.ms - owner.ms if owner and not owner.unknown else -1
            task = replay.tasks.get(owner.task, f"0x{owner.task:08x}") if owner and owner.task else "-"
            site = sym.site(owner, args.frames) if owner else "<no owner>"
            head = f"{merged:7d}{flag}" if position == 0 else " " * 8
            print(f"{head}{size:6d} {age:7d} {task[:15]:<15} {site}")
    print("* freeing this run alone would create a region larger than the current largest free block")
    print_live([a for a in snap.owners.values()], sym, args, f"Used blocks by site at snapshot {snap.snap_id}")



def pinning_runs(snap: Snapshot, max_run: int):
    """Short runs of used blocks that separate two free blocks, with the merged size."""
    ordered = sorted(snap.blocks)
    runs = []
    index = 0
    while index < len(ordered):
        if ordered[index][2]:
            index += 1
            continue
        end = index + 1
        while end < len(ordered) and ordered[end][2]:
            end += 1
        run = ordered[index + 1 : end]
        # Neighbours must be physically adjacent (at most a block header apart), not in another heap region.
        contiguous = all(0 <= b[0] - (a[0] + a[1]) <= 16 for a, b in zip(ordered[index:end], ordered[index + 1 : end + 1]))
        if run and end < len(ordered) and len(run) <= max_run and contiguous:
            merged = ordered[index][1] + sum(b[1] for b in run) + ordered[end][1]
            runs.append((merged, run))
        index = end
    runs.sort(key=lambda item: -item[0])
    return runs


def cmd_pins(args):
    replay, sym = load(args)
    snaps = replay.snapshots
    if args.from_snap:
        snaps = [s for s in snaps if s.snap_id >= int(args.from_snap)]
    if args.to_snap:
        snaps = [s for s in snaps if s.snap_id <= int(args.to_snap)]
    by_site = collections.defaultdict(lambda: {"snaps": set(), "gain": [], "bytes": [], "tasks": set()})
    for snap in snaps:
        largest = snap.stats["largest"]
        for merged, run in pinning_runs(snap, args.max_run):
            if merged <= largest:
                continue
            for ptr, size, _used in run:
                owner = snap.owners.get(ptr)
                site = sym.site(owner, args.frames) if owner else "<no owner>"
                entry = by_site[site]
                entry["snaps"].add(snap.snap_id)
                entry["gain"].append(merged - largest)
                entry["bytes"].append(size)
                if owner and owner.task:
                    entry["tasks"].add(replay.tasks.get(owner.task, f"0x{owner.task:08x}"))
    print(f"{len(snaps)} snapshots; sites whose blocks sit in a run that caps the largest free block")
    print(f"{'snaps':>5} {'max_gain':>8} {'med_gain':>8} {'blk_bytes':>9}  tasks / site")
    ranked = sorted(by_site.items(), key=lambda kv: (-len(kv[1]["snaps"]), -max(kv[1]["gain"])))
    for site, entry in ranked[: args.top]:
        print(f"{len(entry['snaps']):5d} {max(entry['gain']):8d} {int(statistics.median(entry['gain'])):8d} "
              f"{int(statistics.median(entry['bytes'])):9d}  {','.join(sorted(entry['tasks'])) or '-'} / {site}")
    print("gain = how much larger the largest free block would be if that run were freed")


def cmd_churn(args):
    replay, sym = load(args)
    start_ms, end_ms = 0, 1 << 62
    if args.from_snap:
        start_ms = pick_snapshot(replay, args.from_snap).ms
    if args.to_snap:
        end_ms = pick_snapshot(replay, args.to_snap).ms
    by_site = collections.defaultdict(lambda: {"allocs": 0, "frees": 0, "bytes": 0, "lifetimes": []})
    for event in replay.events:
        if not start_ms <= event[1] < end_ms:
            continue
        if event[0] == "A":
            entry = by_site[sym.site(event[2], args.frames)]
            entry["allocs"] += 1
            entry["bytes"] += event[2].size
        elif event[3] is not None:
            entry = by_site[sym.site(event[3], args.frames)]
            entry["frees"] += 1
            entry["lifetimes"].append(event[1] - event[3].ms)
    print(f"window {start_ms}..{'end' if end_ms == 1 << 62 else end_ms} ms")
    print(f"{'allocs':>7} {'frees':>7} {'bytes':>9} {'life_med_ms':>11}  site")
    for site, entry in sorted(by_site.items(), key=lambda kv: -kv[1]["allocs"])[: args.top]:
        life = int(statistics.median(entry["lifetimes"])) if entry["lifetimes"] else -1
        print(f"{entry['allocs']:7d} {entry['frees']:7d} {entry['bytes']:9d} {life:11d}  {site}")


def cmd_map(args):
    replay, sym = load(args)
    snap = pick_snapshot(replay, args.snap)
    bytes_per_px, row_px, row_h, top, left = 4, 1024, 8, 64, 70
    row_bytes = bytes_per_px * row_px
    rows = []
    for start, end in sorted(snap.heaps):
        for row_start in range(start - start % row_bytes, end, row_bytes):
            rows.append(row_start)
    row_index = {row: i for i, row in enumerate(rows)}
    palette = ["#4e79a7", "#f28e2b", "#59a14f", "#b07aa1", "#edc948", "#76b7b2", "#ff9da7", "#9c755f"]
    other_color, free_color, untraced_color = "#8a94a6", "#e4e4e4", "#d65f5f"

    # Colour the sites holding the most bytes; everything else shares one colour.
    site_bytes = collections.Counter()
    for ptr, size, used in snap.blocks:
        owner = snap.owners.get(ptr) if used else None
        if owner is not None and not owner.unknown:
            site_bytes[sym.site(owner, 1)] += size
    free_sorted = sorted((b for b in snap.blocks if not b[2]), key=lambda b: -b[1])
    largest_ptr = free_sorted[0][0] if free_sorted else None
    pins = [run for merged, run in pinning_runs(snap, args.max_run) if merged > snap.stats["largest"]]
    pinned = {block[0] for run in pins for block in run}
    # Sites in capping runs get colours first (largest merged region first), then the sites holding the most bytes.
    ranked = []
    for run in pins:
        for ptr, _size, _used in run:
            owner = snap.owners.get(ptr)
            if owner is not None and not owner.unknown and sym.site(owner, 1) not in ranked:
                ranked.append(sym.site(owner, 1))
    ranked += [site for site, _ in site_bytes.most_common() if site not in ranked]
    site_colors = {site: palette[i] for i, site in enumerate(ranked[: len(palette)])}

    def rects(ptr, size):
        position, remaining = ptr, size
        while remaining > 0:
            row = position - position % row_bytes
            if row not in row_index:
                return
            span = min(remaining, row + row_bytes - position)
            yield (left + (position - row) / bytes_per_px, top + row_index[row] * row_h,
                   max(span / bytes_per_px, 0.6))
            position += span
            remaining -= span

    esc = html.escape
    height = top + len(rows) * row_h + 40 + 14 * (len(site_colors) + 4)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{left + row_px + 20}" height="{height}" '
           f'font-family="Menlo, monospace" font-size="11"><rect width="100%" height="100%" fill="#fff"/>']
    stats = snap.stats
    frag = 1 - stats["largest"] / stats["free"] if stats["free"] else 0
    out.append(f'<text x="{left}" y="18" font-size="14" font-weight="bold">Heap map, snapshot {snap.snap_id} '
               f'@ {snap.ms / 1000:.1f} s</text>')
    out.append(f'<text x="{left}" y="36">free {stats["free"]:,} B · largest {stats["largest"]:,} B · '
               f'fragmentation {frag:.2f} · {stats["free_blocks"]} free / {stats["allocated_blocks"]} used blocks · '
               f'1 px = {bytes_per_px} B, 1 row = {row_bytes} B</text>')
    out.append(f'<text id="info" x="{left}" y="52" fill="#555">Hover a block: address, size, age and call site '
               f'appear here, and blocks from the same site are highlighted. Black outline = largest free block; '
               f'red outline = run that caps it ({len(pins)}).</text>')
    out.append(f'<text id="info2" x="{left}" y="{top - 2}" fill="#b00" font-size="10"></text>')
    for i, row in enumerate(rows):
        out.append(f'<text x="4" y="{top + i * row_h + row_h - 1}" font-size="7" fill="#777">0x{row:08x}</text>')
    overlays = []
    site_ids: dict[str, int] = {}
    for ptr, size, used in sorted(snap.blocks):
        if used:
            owner = snap.owners.get(ptr)
            if owner is None or owner.unknown:
                color, site = untraced_color, "untraced (allocated before tracing or record lost)"
            else:
                site = sym.site(owner, args.frames)
                color = site_colors.get(sym.site(owner, 1), other_color)
            age = f", age {(snap.ms - owner.ms) / 1000:.1f} s" if owner and not owner.unknown else ""
            title = f"used 0x{ptr:08x} {size} B{age}\n{site}"
            site_id = site_ids.setdefault(site, len(site_ids))
        else:
            color, title, site_id = free_color, f"free 0x{ptr:08x} {size} B", -1
        for x, y, w in rects(ptr, size):
            out.append(f'<rect class="b" data-s="{site_id}" x="{x:.1f}" y="{y}" width="{w:.1f}" height="{row_h - 1}" '
                       f'fill="{color}"><title>{esc(title)}</title></rect>')
            if ptr == largest_ptr:
                overlays.append(f'<rect x="{x:.1f}" y="{y}" width="{w:.1f}" height="{row_h - 1}" fill="none" '
                                f'stroke="#000" stroke-width="1.2" pointer-events="none"/>')
            elif ptr in pinned:
                overlays.append(f'<rect x="{x - 1:.1f}" y="{y - 1}" width="{w + 2:.1f}" height="{row_h + 1}" '
                                f'fill="none" stroke="#e00" stroke-width="1.5" pointer-events="none"/>')
    out.extend(overlays)
    legend_y = top + len(rows) * row_h + 24
    items = [(free_color, "free"), (untraced_color, "untraced")]
    items += [(color, site) for site, color in site_colors.items()]
    items.append((other_color, "other call sites"))
    for i, (color, label) in enumerate(items):
        y = legend_y + i * 14
        text = label if len(label) <= 150 else label[:147] + "..."
        out.append(f'<rect x="{left}" y="{y - 9}" width="10" height="10" fill="{color}"/>')
        out.append(f'<text x="{left + 16}" y="{y}">{esc(text)}</text>')
    # Native <title> tooltips are slow and easy to miss on 7 px rows; show details instantly instead.
    out.append("""<style>.b.hl{stroke:#000;stroke-width:1}</style><script><![CDATA[
const info = document.getElementById('info'), info2 = document.getElementById('info2');
const blocks = document.querySelectorAll('.b');
let current = null;
document.addEventListener('mousemove', (event) => {
  const block = event.target.closest && event.target.closest('.b');
  const site = block ? block.dataset.s : null;
  if (block) {
    const lines = block.querySelector('title').textContent.split('\\n');
    info.textContent = lines[0];
    info2.textContent = lines[1] || '';
  }
  if (site === current) return;
  current = site;
  blocks.forEach((b) => b.classList.toggle('hl', site !== null && site !== '-1' && b.dataset.s === site));
});
]]></script>""")
    out.append("</svg>")
    Path(args.out).write_text("\n".join(out))
    print(f"wrote {args.out} ({len(snap.blocks)} blocks, {len(rows)} rows of {row_bytes} bytes, "
          f"{len(pins)} capping runs outlined)")


def main():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--elf", default=".pio/build/heaptrace/firmware.elf", help="firmware ELF for symbols")
    common.add_argument("--frames", type=int, default=1, help="non-allocator frames per site label")
    common.add_argument("--top", type=int, default=25)
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture", help="events.jsonl, serial.bin, or a text log")
    sub = parser.add_subparsers(dest="command", required=True)
    add = lambda name, help: sub.add_parser(name, help=help, parents=[common])  # noqa: E731
    add("summary", "stream health, busiest sites, live memory at end").set_defaults(func=cmd_summary)
    add("snapshots", "per-snapshot fragmentation metrics").set_defaults(func=cmd_snapshots)
    frag = add("frag", "free-block layout and pinning allocations at a snapshot")
    frag.add_argument("--snap", default="last")
    frag.add_argument("--max-run", type=int, default=3, help="longest used-block run counted as pinning")
    frag.set_defaults(func=cmd_frag)
    pins = add("pins", "rank sites that repeatedly split the largest free block across snapshots")
    pins.add_argument("--from-snap")
    pins.add_argument("--to-snap")
    pins.add_argument("--max-run", type=int, default=3)
    pins.set_defaults(func=cmd_pins)
    churn = add("churn", "allocations, frees and lifetimes per site in a window")
    churn.add_argument("--from-snap")
    churn.add_argument("--to-snap")
    churn.set_defaults(func=cmd_churn)
    heap_map = add("map", "SVG block map of a snapshot")
    heap_map.add_argument("--snap", default="last")
    heap_map.add_argument("--out", default="heap-map.svg")
    heap_map.add_argument("--max-run", type=int, default=3)
    heap_map.set_defaults(func=cmd_map)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
