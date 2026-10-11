"""Run with python3 test/test_heap_trace.py (standard library only)."""

import argparse
import base64
import contextlib
import importlib.util
import io
import struct
import sys
import tempfile
import unittest
import xml.dom.minidom
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("heap_trace", ROOT / "scripts/heap_trace.py")
heap_trace = importlib.util.module_from_spec(spec)
sys.modules["heap_trace"] = heap_trace  # dataclasses resolve annotations through sys.modules
spec.loader.exec_module(heap_trace)

USED = 0x80000000
HEAP = 0x3FC90000
TASK = 0x3FCA0000
# Return addresses resolved by the fake symbolizer; names mirror real demangled output.
SYMBOLS = {
    0x42000100: "operator new(unsigned int) (src/util/HeapTrace.cpp:1)",
    0x42000200: "std::unique_ptr<unsigned char []> makeUniqueNoThrow<unsigned char []>(unsigned int) (lib/Memory/Memory.h:33)",
    0x42000300: "unsigned char* freeink::font::psramNewArray<unsigned char>(unsigned int) (FontPsram.h:54)",
    0x42001000: "SdCardFont::fetchAdvancesForCodepoints(unsigned long*, unsigned long, unsigned char) (lib/EpdFont/SdCardFont.cpp:1570)",
    0x42002000: "std::vector<std::basic_string<char>, std::allocator<std::basic_string<char> > >::reserve(unsigned int) (c++/vector.tcc:79)",
    0x42003000: "Page & Layout<int>::emit() (lib/Epub/Page.cpp:7)",
}


def alloc(ptr, size, *callers):
    return b"A" + struct.pack("<IIIB", ptr, size, TASK, len(callers)) + struct.pack(f"<{len(callers)}I", *callers)


def free(ptr):
    return b"F" + struct.pack("<I", ptr)


def snapshot(snap_id, heap_end, blocks, free_bytes, largest):
    """Records for a one-heap, one-pass snapshot; blocks are (ptr, size, used)."""
    records = [
        b"S" + struct.pack("<IBB", snap_id, 1, 6),
        b"N" + struct.pack("<IB", TASK, 4) + b"loop",
        b"H" + struct.pack("<II", HEAP, heap_end),
        b"P" + struct.pack("<III", snap_id, HEAP, HEAP),
    ]
    records += [b"B" + struct.pack("<II", ptr, size | (USED if used else 0)) for ptr, size, used in blocks]
    records.append(b"Q" + struct.pack("<II", heap_end, len(blocks)))
    free_count = sum(1 for block in blocks if not block[2])
    records.append(b"E" + struct.pack("<IIIIII", snap_id, free_bytes, largest, free_bytes, len(blocks) - free_count,
                                      free_count))
    return records


class Stream:
    def __init__(self):
        self.seq = 0
        self.lines = []

    def line(self, records, ms, seq=None, corrupt=False):
        raw = b"".join(records)
        check = heap_trace.fletcher16(raw) ^ (1 if corrupt else 0)
        seq = self.seq if seq is None else seq
        self.lines.append(f"@HT {seq} {ms} {base64.b64encode(raw).decode()} {check:04x}")
        self.seq = seq + 1

    def replay(self):
        replay = heap_trace.Replay()
        for text in self.lines:
            replay.feed_line(text)
        return replay


def fake_resolve(self, addresses):
    for address in addresses:
        self.cache[address] = SYMBOLS.get(address, f"0x{address:08x}")


def fragmented_stream():
    """A heap whose largest hole is split by one long-lived table between two free blocks."""
    stream = Stream()
    table = HEAP + 0x800
    stream.line([alloc(table + 8, 1000, 0x42000300, 0x42001000),
                 alloc(HEAP + 0x40 + 8, 32, 0x42000100, 0x42003000)], 10)
    blocks = [(HEAP, 0x40, False), (HEAP + 0x40, 0x40, True), (HEAP + 0x80, 0x780, False),
              (table, 1016, True), (table + 1016, 0x1000 - 0x800 - 1016, False)]
    stream.line(snapshot(1, HEAP + 0x1000, blocks, 0x40 + 0x780 + (0x800 - 1016), 0x780), 20)
    return stream


class ReplayTest(unittest.TestCase):
    def test_stream_health(self):
        stream = Stream()
        stream.line([alloc(HEAP + 8, 16)], 1)
        stream.line([alloc(HEAP + 40, 16)], 2, corrupt=True)
        stream.line([alloc(HEAP + 80, 16)], 3, seq=5)
        replay = stream.replay()
        self.assertEqual((replay.lines, replay.bad_lines, replay.seq_gaps), (2, 1, 1))
        self.assertEqual(len(replay.live.allocs), 2)

        stream.line([alloc(HEAP + 120, 16)], 1, seq=0)
        replay = stream.replay()
        self.assertEqual(replay.boots, 1)
        self.assertEqual(list(replay.live.allocs), [HEAP + 120])

    def test_overlapping_alloc_evicts_stale_entry(self):
        stream = Stream()
        stream.line([alloc(HEAP + 8, 100), alloc(HEAP + 40, 50)], 1)
        replay = stream.replay()
        self.assertEqual(list(replay.live.allocs), [HEAP + 40])
        self.assertEqual(replay.evicted, 1)

    def test_snapshot_reconciles_replay(self):
        stream = Stream()
        stream.line([alloc(HEAP + 0x100 + 8, 24), alloc(HEAP + 0x200 + 8, 24)], 1)
        # The walk shows an untraced block at HEAP and the block at 0x200 already free.
        blocks = [(HEAP, 32, True), (HEAP + 32, 0x100 - 32, False), (HEAP + 0x100, 32, True),
                  (HEAP + 0x120, 0x1000 - 0x120, False)]
        stream.line(snapshot(1, HEAP + 0x1000, blocks, 0x1000 - 64, 0x1000 - 0x120), 2)
        # Freeing the untraced block by its user pointer (block + poison header) removes it.
        stream.line([free(HEAP + 8)], 3)
        replay = stream.replay()
        snap = replay.snapshots[0]
        self.assertEqual((snap.verified, snap.added_unknown, snap.removed_stale), (1, 1, 1))
        self.assertEqual(list(replay.live.allocs), [HEAP + 0x100 + 8])
        self.assertEqual(replay.unmatched_frees, 0)
        self.assertEqual(replay.tasks[TASK], "loop")


class AnalysisTest(unittest.TestCase):
    def test_pinning_runs(self):
        snap = heap_trace.Snapshot(1, 0, blocks=[
            (0x1000, 100, False), (0x1064, 16, True), (0x1074, 200, False),
            (0x113C, 8, True), (0x1144, 8, True), (0x114C, 50, False),
            (0x1180, 8, True), (0x1188, 8, True), (0x1190, 8, True), (0x1198, 8, True), (0x11A0, 60, False),
            (0x2000, 40, False), (0x2028, 8, True), (0x3000, 40, False),
        ])
        merged = sorted(size for size, _ in heap_trace.pinning_runs(snap, max_run=3))
        # Four used blocks exceed max_run; the block before a region gap does not merge.
        self.assertEqual(merged, [266, 316])

    def test_site_skips_allocator_frames(self):
        symbolizer = heap_trace.Symbolizer(None)
        fake_resolve(symbolizer, SYMBOLS)
        wrapped = heap_trace.Alloc(1, 8, 0, (0x42000100, 0x42000200, 0x42000300, 0x42001000), 0)
        self.assertTrue(symbolizer.site(wrapped).startswith("SdCardFont::fetchAdvancesForCodepoints"))
        untraced = heap_trace.Alloc(1, 8, 0, (), 0, unknown=True)
        self.assertIn("untraced", symbolizer.site(untraced))


class LocationTest(unittest.TestCase):
    def test_labels_show_repo_relative_paths(self):
        # addr2line output from a heaptrace ELF, with the home directory replaced.
        cases = {
            "/home/dev/crosspoint-reader/lib/EpdFont/SdCardFont.cpp:1478": "lib/EpdFont/SdCardFont.cpp:1478",
            "/home/dev/crosspoint-reader/freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp:197":
                "freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp:197",
            "/home/dev/.platformio/packages/framework-espidf/components/pthread/pthread.c:623":
                "framework-espidf/components/pthread/pthread.c:623",
            "/home/dev/.platformio/packages/framework-arduinoespressif32/cores/esp32/WString.cpp:185":
                "arduino/cores/esp32/WString.cpp:185",
            "/home/dev/.platformio/packages/framework-arduinoespressif32/libraries/Wire/src/Wire.cpp:453 "
            "(discriminator 2)": "arduino/libraries/Wire/src/Wire.cpp:453",
            "/home/dev/.platformio/packages/toolchain-riscv32-esp/riscv32-esp-elf/include/c++/14.2.0/bits/"
            "basic_string.tcc:332 (discriminator 1)": "c++/basic_string.tcc:332",
            # libstdc++ headers keep the toolchain build machine's path.
            "/builds/idf/crosstool-NG/.build/riscv32-esp-elf/build/build-cc-gcc-final/riscv32-esp-elf/"
            "rv32imc_zicsr_zifencei/ilp32/no-rtti/libstdc++-v3/include/riscv32-esp-elf/bits/gthr-default.h:746":
                "gthr-default.h:746",
        }
        for raw, label in cases.items():
            self.assertEqual(heap_trace.clean_location(raw), label)


class CommandTest(unittest.TestCase):
    def run_command(self, stream, *argv):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.txt"
            capture.write_text("\n".join(stream.lines) + "\n")
            out = Path(directory) / "map.svg"
            argv = [str(capture), *argv, "--elf", str(Path(directory) / "missing.elf")]
            if argv[1] == "map":
                argv += ["--out", str(out)]
            printed = io.StringIO()
            with mock.patch.object(heap_trace.Symbolizer, "resolve", fake_resolve), \
                    mock.patch("sys.argv", ["heap_trace.py", *argv]), contextlib.redirect_stdout(printed):
                heap_trace.main()
            return printed.getvalue(), out.read_text() if out.exists() else None

    def test_map_is_valid_interactive_svg(self):
        _, svg = self.run_command(fragmented_stream(), "map", "--snap", "1")
        document = xml.dom.minidom.parseString(svg)
        self.assertEqual(len(document.getElementsByTagName("script")), 1)
        legend = [text.firstChild.data for text in document.getElementsByTagName("text") if text.firstChild]
        # The pinning site is the first coloured legend entry after free/untraced.
        self.assertTrue(legend[legend.index("untraced") + 1].startswith("SdCardFont::fetchAdvancesForCodepoints"))
        self.assertIn("Page &amp; Layout&lt;int&gt;::emit()", svg)
        outlines = [rect for rect in document.getElementsByTagName("rect") if rect.getAttribute("fill") == "none"]
        self.assertEqual({rect.getAttribute("stroke") for rect in outlines}, {"#000", "#e00"})

    def test_pins_and_frag_report_the_splitter(self):
        pins, _ = self.run_command(fragmented_stream(), "pins")
        self.assertIn("SdCardFont::fetchAdvancesForCodepoints", pins.splitlines()[2])
        frag, _ = self.run_command(fragmented_stream(), "frag", "--snap", "1")
        self.assertIn("merged", frag)
        self.assertIn("*", frag.split("Pinning runs", 1)[1].splitlines()[2])

    def test_summary_and_snapshots_run(self):
        summary, _ = self.run_command(fragmented_stream(), "summary")
        self.assertIn("lines=2 bad=0 seq_gaps=0", summary)
        snapshots, _ = self.run_command(fragmented_stream(), "snapshots")
        self.assertIn("0.", snapshots.splitlines()[1])


if __name__ == "__main__":
    unittest.main()
