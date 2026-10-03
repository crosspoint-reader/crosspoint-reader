#!/usr/bin/env python3
# Pocket Library for CrossPoint
# Copyright (C) 2026 Pocket Library contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version. See LICENSE-GPL-3.0 at the repository root.
"""Pocket Library card builder.

Turns library.toml (which collections you want) into a ready microSD card:

  plan      look each collection up in the Kiwix catalog; show sizes and
            whether everything fits. Reads nothing big, writes nothing.
  download  fetch the newest edition of each collection into the staging
            folder, resuming interrupted downloads, and check its SHA-256.
  index     build the search index (.pltitles) for each one with zimindex.
  copy      put everything on the card under /library: ZIMs over 4,000 MiB
            are written as 4,000 MiB parts (.zimaa, .zimab, ...), then
            manifest.json. --dry-run shows what would happen.
  all       download, index, copy.

Every step can be re-run: finished work is recognised and skipped. Nothing
on the card is deleted unless you pass --prune (old editions are listed).

Standard library only (Python 3.11+), so nothing needs installing.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import tomllib
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

VERSION = "0.1.0"
CATALOG = "https://library.kiwix.org/catalog/v2/entries"
PART_SIZE = 4000 * 1024 * 1024  # 4,000 MiB: under FAT32's 4 GiB file limit
CARD_DIR = "library"
MANIFEST = "manifest.json"
STATE_FILE = ".cardbuilder-state.json"
CHUNK = 4 * 1024 * 1024
USER_AGENT = f"PocketLibraryCardBuilder/{VERSION}"


class BuildError(Exception):
    """A problem the user needs to see; printed without a traceback."""


# --- small helpers ------------------------------------------------------------


def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if abs(n) < 1000 or unit == "TB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1000
    return f"{n:.1f} TB"


def say(msg: str = "") -> None:
    print(msg, flush=True)


class Progress:
    """One self-overwriting status line, at most a few times a second."""

    def __init__(self, label: str, total: int, start: int = 0):
        self.label, self.total, self.start = label, total, start
        self.t0 = time.monotonic()
        self.last = 0.0

    def update(self, done: int, force: bool = False) -> None:
        now = time.monotonic()
        if not force and now - self.last < 0.5:
            return
        self.last = now
        rate = (done - self.start) / max(now - self.t0, 1e-6)
        pct = 100.0 * done / self.total if self.total else 100.0
        eta = ""
        if rate > 0 and self.total > done:
            secs = int((self.total - done) / rate)
            eta = f", {secs // 3600}h{(secs // 60) % 60:02d}m left"
        sys.stdout.write(f"\r  {self.label}: {pct:5.1f}%  {human(done)} / {human(self.total)}  "
                         f"{human(rate)}/s{eta}      ")
        sys.stdout.flush()

    def finish(self, done: int) -> None:
        self.update(done, force=True)
        sys.stdout.write("\n")
        sys.stdout.flush()


def sha256_file(path: Path, label: str = "checking") -> str:
    h = hashlib.sha256()
    size = path.stat().st_size
    prog = Progress(f"{label} {path.name}", size)
    done = 0
    with open(path, "rb") as f:
        while chunk := f.read(CHUNK):
            h.update(chunk)
            done += len(chunk)
            prog.update(done)
    prog.finish(done)
    return h.hexdigest()


def read_zim_uuid(path: Path) -> str:
    with open(path, "rb") as f:
        head = f.read(24)
    if len(head) < 24 or struct.unpack("<I", head[:4])[0] != 0x044D495A:
        raise BuildError(f"{path} is not a ZIM file")
    return head[8:24].hex()


def read_index_header(path: Path) -> dict:
    with open(path, "rb") as f:
        head = f.read(60)
    if len(head) < 60 or head[:8] != b"PLTITLE\0":
        raise BuildError(f"{path} is not a Pocket Library title index")
    version, page, fold, records = struct.unpack("<IIII", head[8:24])
    return {"version": version, "fold_version": fold, "records": records,
            "zim_uuid": head[40:56].hex(), "zim_entries": struct.unpack("<I", head[56:60])[0]}


# --- library.toml ---------------------------------------------------------------


@dataclass
class Want:
    key: str            # folder name on the card
    name: str           # Kiwix catalog name, e.g. wikipedia_en_all
    flavours: list      # preferred first, e.g. ["nopic", "mini"]
    lang: str = ""
    optional: bool = False
    url: str = ""       # direct .zim URL instead of the catalog
    note: str = ""


def load_library(path: Path) -> tuple[dict, list[Want]]:
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise BuildError(f"{path} not found")
    except tomllib.TOMLDecodeError as e:
        raise BuildError(f"{path}: {e}")
    settings = data.get("settings", {})
    wants = []
    seen = set()
    for i, c in enumerate(data.get("collection", [])):
        name = c.get("name", "")
        url = c.get("url", "")
        if not name and not url:
            raise BuildError(f"{path}: collection #{i + 1} needs a name or a url")
        key = c.get("key") or name or Path(urllib.parse.urlparse(url).path).stem
        if key in seen:
            raise BuildError(f"{path}: two collections use the folder name {key!r}")
        seen.add(key)
        fl = c.get("flavour", [])
        wants.append(Want(key=key, name=name, flavours=[fl] if isinstance(fl, str) else list(fl),
                          lang=c.get("lang", ""), optional=bool(c.get("optional", False)),
                          url=url, note=c.get("note", "")))
    if not wants:
        raise BuildError(f"{path} lists no [[collection]] entries")
    return settings, wants


# --- catalog ----------------------------------------------------------------------


@dataclass
class Edition:
    want: Want
    title: str
    name: str
    flavour: str
    language: str
    date: str          # YYYY-MM-DD
    url: str           # direct .zim URL
    size: int          # bytes (0 if the catalog did not say)
    category: str = ""
    description: str = ""

    @property
    def filename(self) -> str:
        return Path(urllib.parse.urlparse(self.url).path).name


def http_get(url: str, timeout: float = 60) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def parse_catalog(xml_bytes: bytes) -> list[dict]:
    """Entries of a Kiwix OPDS v2 feed as plain dicts."""
    root = ET.fromstring(xml_bytes)
    out = []
    for entry in root:
        if _local(entry.tag) != "entry":
            continue
        e: dict = {}
        for child in entry:
            tag = _local(child.tag)
            if tag == "link":
                href = child.get("href", "")
                if child.get("type") == "application/x-zim" or href.endswith((".zim", ".zim.meta4")):
                    e["href"] = href
                    e["length"] = int(child.get("length") or 0)
            elif tag in ("author", "publisher"):
                n = next((c.text for c in child if _local(c.tag) == "name"), "")
                e[tag] = n or ""
            else:
                e[tag] = (child.text or "").strip()
        if e.get("href"):
            out.append(e)
    return out


def _edition_date(e: dict, href: str) -> str:
    for k in ("issued", "updated"):
        if e.get(k):
            return e[k][:10]
    stem = Path(urllib.parse.urlparse(href).path).name
    return stem.rsplit("_", 1)[-1].split(".")[0]


def resolve(want: Want, catalog: str = CATALOG) -> Edition | None:
    if want.url:
        fn = Path(urllib.parse.urlparse(want.url).path).name
        return Edition(want, title=want.key, name=want.name or want.key, flavour="", language=want.lang,
                       date=fn.rsplit("_", 1)[-1].split(".")[0], url=want.url, size=0)
    query = {"name": want.name, "count": "-1"}
    if want.lang:
        query["lang"] = want.lang
    url = catalog + "?" + urllib.parse.urlencode(query)
    try:
        entries = parse_catalog(http_get(url))
    except (urllib.error.URLError, OSError) as e:
        raise BuildError(f"cannot reach the Kiwix catalog ({url}): {e}")
    except ET.ParseError as e:
        raise BuildError(f"the Kiwix catalog sent something unreadable for {want.name}: {e}")
    entries = [e for e in entries if e.get("name", want.name) == want.name]
    order = want.flavours or [""]
    for fl in order:
        matches = [e for e in entries if not fl or e.get("flavour", "") == fl]
        if not matches:
            continue
        best = max(matches, key=lambda e: _edition_date(e, e["href"]))
        href = best["href"]
        if href.endswith(".meta4"):
            href = href[: -len(".meta4")]
        return Edition(want, title=best.get("title", want.name), name=want.name,
                       flavour=best.get("flavour", fl), language=best.get("language", want.lang),
                       date=_edition_date(best, href), url=href, size=best.get("length", 0),
                       category=best.get("category", ""), description=best.get("summary", ""))
    return None


def resolve_all(wants: list[Want], catalog: str) -> list[Edition]:
    out = []
    for w in wants:
        ed = resolve(w, catalog)
        if ed is None:
            msg = f"{w.name}: no edition with flavour {' or '.join(w.flavours) or 'any'} in the catalog"
            if w.optional:
                say(f"  skipped (optional) {msg}")
                continue
            raise BuildError(msg + " (mark it optional = true to skip it)")
        out.append(ed)
    return out


# --- staging state ------------------------------------------------------------------


class State:
    """Remembers which staged files were verified, so a 50 GB file is hashed once."""

    def __init__(self, staging: Path):
        self.path = staging / STATE_FILE
        try:
            self.data = json.loads(self.path.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            self.data = {}

    def verified(self, f: Path) -> str | None:
        rec = self.data.get(f.name)
        if not rec or not f.exists():
            return None
        st = f.stat()
        if rec.get("size") == st.st_size and rec.get("mtime") == int(st.st_mtime):
            return rec.get("sha256")
        return None

    def mark(self, f: Path, sha: str) -> None:
        st = f.stat()
        self.data[f.name] = {"size": st.st_size, "mtime": int(st.st_mtime), "sha256": sha}
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.data, indent=1, sort_keys=True))
        os.replace(tmp, self.path)


# --- download -------------------------------------------------------------------------


def fetch_checksum(url: str) -> str:
    try:
        text = http_get(url + ".sha256").decode("ascii", "replace")
    except (urllib.error.URLError, OSError) as e:
        raise BuildError(f"cannot fetch the checksum {url}.sha256: {e}")
    digest = text.strip().split()[0] if text.strip() else ""
    if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest.lower()):
        raise BuildError(f"{url}.sha256 does not contain a SHA-256 checksum")
    return digest.lower()


def download(ed: Edition, staging: Path, state: State, retries: int = 8) -> Path:
    dest = staging / ed.filename
    expected = fetch_checksum(ed.url)
    if dest.exists():
        known = state.verified(dest)
        if known == expected:
            say(f"  {dest.name}: already downloaded and checked")
            return dest
        say(f"  {dest.name}: found in the staging folder; checking it")
        got = sha256_file(dest)
        if got == expected:
            state.mark(dest, got)
            return dest
        raise BuildError(f"{dest} does not match its published checksum. Move it away (it is not "
                         f"deleted automatically) and run download again.")

    part = dest.with_name(dest.name + ".part")
    attempt = 0
    while True:
        have = part.stat().st_size if part.exists() else 0
        headers = {"User-Agent": USER_AGENT}
        if have:
            headers["Range"] = f"bytes={have}-"
        req = urllib.request.Request(ed.url, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                if have and r.status != 206:
                    have = 0  # server ignored the range: start over
                length = r.headers.get("Content-Length")
                total = have + int(length) if length else ed.size
                prog = Progress(f"downloading {dest.name}", total, have)
                with open(part, "ab" if have else "wb") as f:
                    done = have
                    while chunk := r.read(CHUNK):
                        f.write(chunk)
                        done += len(chunk)
                        prog.update(done)
                prog.finish(done)
                if total and done < total:
                    raise urllib.error.URLError(f"connection closed at {done} of {total} bytes")
            break
        except urllib.error.HTTPError as e:
            if e.code == 416 and have:  # range past the end: the part is already whole
                break
            if e.code in (404, 410):
                raise BuildError(f"{ed.url} is gone ({e.code}); run plan again for the newest edition")
            err = e
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            err = e
        attempt += 1
        if attempt > retries:
            raise BuildError(f"download of {ed.url} keeps failing ({err}); run download again to resume")
        wait = min(2 ** attempt, 120)
        say(f"\n  interrupted ({err}); resuming in {wait} s (try {attempt} of {retries})")
        time.sleep(wait)

    got = sha256_file(part)
    if got != expected:
        bad = part.with_name(part.name + ".bad")
        os.replace(part, bad)
        raise BuildError(f"{ed.filename} does not match its published checksum; kept as {bad.name}. "
                         f"Delete it and run download again.")
    os.replace(part, dest)
    state.mark(dest, got)
    say(f"  {dest.name}: checksum OK")
    return dest


# --- index ----------------------------------------------------------------------------------


def find_tool(name: str, explicit: str | None) -> Path:
    cands = [Path(explicit)] if explicit else []
    here = Path(__file__).resolve().parent
    cands += [here / name, Path.cwd() / name]
    found = shutil.which(name)
    if found:
        cands.append(Path(found))
    for c in cands:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    raise BuildError(f"cannot find {name}. Put it next to cardbuilder.py (from the dev release) "
                     f"or pass --{name} PATH")


def index_path(zim: Path) -> Path:
    return zim.with_suffix(".pltitles")


def build_index(zim: Path, zimindex: Path) -> Path:
    out = index_path(zim)
    if out.exists() and out.stat().st_mtime >= zim.stat().st_mtime:
        try:
            if read_index_header(out)["zim_uuid"] == read_zim_uuid(zim):
                say(f"  {out.name}: already built")
                return out
        except BuildError:
            pass
    say(f"  building {out.name} (large collections take several minutes)")
    r = subprocess.run([str(zimindex), str(zim), str(out)])
    if r.returncode != 0:
        raise BuildError(f"zimindex failed on {zim.name} (exit {r.returncode})")
    return out


# --- copy to card ------------------------------------------------------------------------------


@dataclass
class Item:
    """One file to place on the card: a byte range of a staged file."""
    src: Path
    offset: int
    size: int
    dest: Path


@dataclass
class Placement:
    ed: Edition
    zim: Path
    index: Path
    sha256: str
    items: list = field(default_factory=list)


def part_names(stem: str, count: int) -> list[str]:
    names = []
    for i in range(count):
        a, b = divmod(i, 26)
        if a >= 26:
            raise BuildError(f"{stem} needs more than 676 parts")
        names.append(f"{stem}.zim{chr(97 + a)}{chr(97 + b)}")
    return names


def plan_placement(ed: Edition, zim: Path, sha: str, card: Path, part_size: int) -> Placement:
    folder = card / CARD_DIR / ed.want.key
    size = zim.stat().st_size
    pl = Placement(ed, zim, index_path(zim), sha)
    if size > part_size:
        count = -(-size // part_size)
        for i, n in enumerate(part_names(zim.stem, count)):
            pl.items.append(Item(zim, i * part_size, min(part_size, size - i * part_size), folder / n))
    else:
        pl.items.append(Item(zim, 0, size, folder / zim.name))
    pl.items.append(Item(pl.index, 0, pl.index.stat().st_size, folder / pl.index.name))
    return pl


def item_done(it: Item) -> bool:
    return it.dest.exists() and it.dest.stat().st_size == it.size


def copy_item(it: Item, verify: bool) -> None:
    it.dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = it.dest.with_name(it.dest.name + ".tmp")
    prog = Progress(f"copying {it.dest.name}", it.size)
    h = hashlib.sha256() if verify else None
    done = 0
    with open(it.src, "rb") as fin, open(tmp, "wb") as fout:
        fin.seek(it.offset)
        while done < it.size:
            chunk = fin.read(min(CHUNK, it.size - done))
            if not chunk:
                raise BuildError(f"{it.src} ended early")
            fout.write(chunk)
            if h:
                h.update(chunk)
            done += len(chunk)
            prog.update(done)
        fout.flush()
        os.fsync(fout.fileno())
    prog.finish(done)
    if h:
        back = hashlib.sha256()
        with open(tmp, "rb") as f:
            while chunk := f.read(CHUNK):
                back.update(chunk)
        if back.digest() != h.digest():
            os.remove(tmp)
            raise BuildError(f"{it.dest} read back differently from what was written; the card may be failing")
    os.replace(tmp, it.dest)


def manifest_for(placements: list[Placement], card: Path) -> dict:
    cols = []
    for pl in placements:
        ix = read_index_header(pl.index)
        zim_items = [it for it in pl.items if it.src == pl.zim]
        cols.append({
            "key": pl.ed.want.key,
            "title": pl.ed.title,
            "name": pl.ed.name,
            "flavour": pl.ed.flavour,
            "language": pl.ed.language,
            "category": pl.ed.category,
            "date": pl.ed.date,
            "description": pl.ed.description,
            "zim": {
                "file": pl.zim.name,
                "size": pl.zim.stat().st_size,
                "sha256": pl.sha256,
                "uuid": read_zim_uuid(pl.zim),
                "parts": [{"path": str(it.dest.relative_to(card)), "size": it.size} for it in zim_items],
            },
            "index": {"path": str(pl.items[-1].dest.relative_to(card)), "size": pl.items[-1].size,
                      "records": ix["records"], "fold_version": ix["fold_version"]},
        })
    return {"format": 1, "builder": VERSION,
            "built": _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat(),
            "collections": cols}


def stale_files(placements: list[Placement], card: Path) -> list[Path]:
    """Files under /library that this build does not produce (older editions, leftovers)."""
    keep = {it.dest for pl in placements for it in pl.items}
    keep.add(card / CARD_DIR / MANIFEST)
    root = card / CARD_DIR
    out = []
    if root.exists():
        for p in sorted(root.rglob("*")):
            if p.is_file() and p not in keep and not p.name.startswith("._") and p.name != ".DS_Store":
                out.append(p)
    return out


def copy_to_card(placements: list[Placement], card: Path, dry_run: bool, verify: bool, prune: bool) -> None:
    if not card.is_dir():
        raise BuildError(f"card folder {card} not found. Is the card mounted? (ls /Volumes)")
    todo = [it for pl in placements for it in pl.items if not item_done(it)]
    stale = stale_files(placements, card)
    need = sum(it.size for it in todo)
    freed = sum(p.stat().st_size for p in stale) if prune else 0
    free = shutil.disk_usage(card).free
    say(f"Card {card}: {human(free)} free; {len(todo)} file(s) to write, {human(need)}.")
    for it in todo:
        say(f"  write  {it.dest.relative_to(card)}  ({human(it.size)})")
    for p in stale:
        say(f"  {'remove' if prune else 'stale '} {p.relative_to(card)}  ({human(p.stat().st_size)})")
    if stale and not prune:
        say("  (stale files are left alone; add --prune to remove them)")
    if need > free + freed:
        raise BuildError(f"not enough space on the card: need {human(need)}, have {human(free + freed)}")
    if dry_run:
        say("Dry run: nothing written.")
        return
    if prune:
        for p in stale:
            p.unlink()
    for it in todo:
        copy_item(it, verify)
    man = manifest_for(placements, card)
    tmp = card / CARD_DIR / (MANIFEST + ".tmp")
    tmp.parent.mkdir(parents=True, exist_ok=True)
    tmp.write_text(json.dumps(man, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, card / CARD_DIR / MANIFEST)
    say(f"Wrote {CARD_DIR}/{MANIFEST}. Card is ready; eject it before unplugging.")


# --- commands ---------------------------------------------------------------------------------------


def cmd_plan(eds: list[Edition], staging: Path, card: Path | None) -> None:
    total = 0
    say(f"{'Collection':<22} {'Edition':<44} {'Size':>9}  Staged")
    for ed in eds:
        staged = (staging / ed.filename).exists()
        total += ed.size
        say(f"{ed.want.key:<22} {ed.filename:<44} {human(ed.size):>9}  {'yes' if staged else 'no'}")
    say(f"{'Total':<67} {human(total):>9}")
    say(f"Search indexes add roughly 0.5% on top.")
    st_free = shutil.disk_usage(staging).free if staging.exists() else 0
    missing = sum(ed.size for ed in eds if not (staging / ed.filename).exists())
    say(f"Staging {staging}: {human(st_free)} free, {human(missing)} still to download"
        f"{'  << NOT ENOUGH' if missing > st_free else ''}.")
    if card:
        if card.is_dir():
            free = shutil.disk_usage(card).free
            say(f"Card {card}: {human(free)} free, collections need about {human(total * 1.005)}"
                f"{'  << NOT ENOUGH' if total * 1.005 > free else ''} (files already on the card count as free here).")
        else:
            say(f"Card {card}: not mounted.")


def staged_zims(eds: list[Edition], staging: Path, state: State) -> tuple[list[tuple[Edition, Path, str]], list[Edition]]:
    """Collections ready to index or copy, and the ones skipped because they
    are not downloaded and checked yet."""
    out, missing = [], []
    for ed in eds:
        z = staging / ed.filename
        sha = state.verified(z)
        if sha:
            out.append((ed, z, sha))
        else:
            missing.append(ed)
    for ed in missing:
        say(f"[{ed.want.key}] skipped: {ed.filename} is not downloaded and checked yet (run download)")
    if eds and not out:
        raise BuildError("nothing is downloaded and checked yet; run download first")
    return out, missing


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="cardbuilder", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["plan", "download", "index", "copy", "all"])
    ap.add_argument("--library", default=str(Path(__file__).resolve().parent / "library.toml"),
                    help="collections to build (default: library.toml next to this script)")
    ap.add_argument("--staging", help="folder for downloads and indexes (default: settings.staging)")
    ap.add_argument("--card", help="the mounted card, e.g. /Volumes/POCKETLIB (default: settings.card)")
    ap.add_argument("--only", action="append", help="limit to this collection key (repeatable)")
    ap.add_argument("--dry-run", action="store_true", help="copy: show what would be written; write nothing")
    ap.add_argument("--verify", action="store_true", help="copy: read every file back and compare")
    ap.add_argument("--prune", action="store_true", help="copy: delete files under /library this build does not use")
    ap.add_argument("--zimindex", help="path to the zimindex tool")
    ap.add_argument("--catalog", default=CATALOG, help=argparse.SUPPRESS)
    ap.add_argument("--part-size", type=int, default=PART_SIZE, help=argparse.SUPPRESS)
    a = ap.parse_args(argv)

    try:
        settings, wants = load_library(Path(a.library))
        if a.only:
            unknown = set(a.only) - {w.key for w in wants}
            if unknown:
                raise BuildError(f"--only: no collection called {', '.join(sorted(unknown))}")
            wants = [w for w in wants if w.key in a.only]
        staging = Path(os.path.expanduser(a.staging or settings.get("staging", "")))
        if not str(staging) or str(staging) == ".":
            raise BuildError("say where downloads go: --staging FOLDER or settings.staging in library.toml")
        card_s = a.card or settings.get("card", "")
        card = Path(os.path.expanduser(card_s)) if card_s else None

        say(f"Looking up {len(wants)} collection(s) in the Kiwix catalog...")
        eds = resolve_all(wants, a.catalog)
        if a.command == "plan":
            cmd_plan(eds, staging, card)
            return 0

        if not staging.is_dir():
            raise BuildError(f"staging folder {staging} not found (is the drive plugged in?)")
        state = State(staging)
        if a.command in ("download", "all"):
            for ed in eds:
                say(f"[{ed.want.key}] {ed.filename}")
                download(ed, staging, state)
        if a.command in ("index", "all"):
            tool = find_tool("zimindex", a.zimindex)
            ready, _ = staged_zims(eds, staging, state)
            for ed, z, _ in ready:
                say(f"[{ed.want.key}]")
                build_index(z, tool)
        if a.command in ("copy", "all"):
            if not card:
                raise BuildError("say where the card is: --card /Volumes/NAME or settings.card in library.toml")
            placements = []
            ready, missing = staged_zims(eds, staging, state)
            prune = a.prune and not a.only
            if missing and prune:
                say("Not pruning: some collections were skipped, and their files on the card would look stale.")
                prune = False
            for ed, z, sha in ready:
                if not index_path(z).exists():
                    raise BuildError(f"no search index for {z.name}; run index first")
                placements.append(plan_placement(ed, z, sha, card, a.part_size))
            if a.only:
                say("Note: with --only, files of other collections on the card show as stale.")
            copy_to_card(placements, card, a.dry_run, a.verify, prune)
        return 0
    except BuildError as e:
        say(f"\ncardbuilder: {e}")
        return 1
    except KeyboardInterrupt:
        say("\nStopped. Run the same command again to pick up where it left off.")
        return 130


if __name__ == "__main__":
    sys.exit(main())
