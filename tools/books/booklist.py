#!/usr/bin/env python3
# Pocket Library for CrossPoint
# Copyright (C) 2026 Pocket Library contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version. See LICENSE-GPL-3.0 at the repository root.
"""Sort a big folder of ebooks before they go on the card.

    python3 booklist.py list FOLDER            -> books.csv on the Desktop
    python3 booklist.py convert books.csv OUT  -> EPUBs of the rows marked "keep"

`list` reads each book's title, author, year and language with Calibre's
ebook-meta (every .mobi, .azw, .azw3, .epub, .prc under FOLDER, subfolders
included) and writes one row per book, sorted by author then title. Books
that look like copies of an earlier row (same title and author, ignoring
case, punctuation and "The") are marked in the "duplicate of" column.

Open books.csv in Numbers or Excel, put anything (x, y, 1) in the "keep"
column of the books you want, and save it as CSV again. `convert` then makes
an EPUB of each kept book with Calibre's ebook-convert (EPUBs are copied as
they are) into OUT, skipping any already there, so it can be stopped and run
again. Standard library only; needs Calibre in /Applications.
"""

import csv
import os
import re
import shutil
import subprocess
import sys
import unicodedata
from concurrent.futures import ThreadPoolExecutor

CALIBRE = "/Applications/calibre.app/Contents/MacOS"
EXTENSIONS = (".mobi", ".azw", ".azw3", ".prc", ".epub")
COLUMNS = ["keep", "author", "title", "year", "language", "format", "size MB", "duplicate of", "file"]


def tool(name):
    path = os.path.join(CALIBRE, name)
    if os.path.exists(path):
        return path
    found = shutil.which(name)
    if found:
        return found
    sys.exit(f"Calibre's {name} was not found. Install Calibre from https://calibre-ebook.com/download_osx "
             "and drag it into Applications.")


def parse_meta(text):
    """ebook-meta's "Key : value" lines -> {title, author, year, language}."""
    fields = {}
    for line in text.splitlines():
        if ":" not in line:
            continue
        key, _, value = line.partition(":")
        fields[key.strip().lower()] = value.strip()
    author = fields.get("author(s)", "")
    author = re.sub(r"\s*\[[^\]]*\]", "", author)  # "Jane Austen [Austen, Jane]"
    year = ""
    m = re.search(r"\b(1[0-9]{3}|20[0-9]{2})\b", fields.get("published", ""))
    if m and not fields.get("published", "").startswith("0101"):
        year = m.group(1)
    return {
        "title": fields.get("title", ""),
        "author": author.replace(" & ", "; "),
        "year": year,
        "language": fields.get("languages", ""),
    }


def match_key(title, author):
    """Same book despite case, accents, punctuation, a leading article."""
    def norm(s):
        s = unicodedata.normalize("NFKD", s.lower())
        s = "".join(c for c in s if not unicodedata.combining(c))
        s = re.sub(r"[^a-z0-9 ]+", " ", s)
        s = re.sub(r"^(the|a|an) ", "", s.strip())
        return " ".join(s.split())
    return norm(title) + "|" + norm(author)


def read_one(meta_tool, path):
    try:
        out = subprocess.run([meta_tool, path], capture_output=True, text=True, timeout=120).stdout
    except subprocess.TimeoutExpired:
        out = ""
    info = parse_meta(out)
    if not info["title"]:
        info["title"] = os.path.splitext(os.path.basename(path))[0]
    info["format"] = os.path.splitext(path)[1][1:].lower()
    info["size MB"] = f"{os.path.getsize(path) / 1e6:.1f}"
    info["file"] = path
    return info


def cmd_list(folder):
    folder = os.path.abspath(os.path.expanduser(folder))
    if not os.path.isdir(folder):
        sys.exit(f"Not a folder: {folder}")
    paths = []
    for root, _, files in os.walk(folder):
        for name in files:
            if name.lower().endswith(EXTENSIONS) and not name.startswith("."):
                paths.append(os.path.join(root, name))
    if not paths:
        sys.exit(f"No .mobi/.azw3/.epub files under {folder}")
    print(f"Reading {len(paths)} books (a second or so each, four at a time)...")
    meta_tool = tool("ebook-meta")
    rows = []
    with ThreadPoolExecutor(max_workers=4) as pool:
        for i, info in enumerate(pool.map(lambda p: read_one(meta_tool, p), paths), 1):
            rows.append(info)
            if i % 25 == 0 or i == len(paths):
                print(f"  {i}/{len(paths)}")
    rows.sort(key=lambda r: (r["author"].lower(), r["title"].lower(), r["format"] != "epub"))
    seen = {}
    for r in rows:
        k = match_key(r["title"], r["author"])
        r["duplicate of"] = seen.get(k, "")
        seen.setdefault(k, os.path.basename(r["file"]))
        r["keep"] = ""
    out = os.path.expanduser("~/Desktop/books.csv")
    with open(out, "w", newline="", encoding="utf-8-sig") as f:  # BOM: Excel reads the accents
        w = csv.DictWriter(f, fieldnames=COLUMNS)
        w.writeheader()
        w.writerows(rows)
    dups = sum(1 for r in rows if r["duplicate of"])
    print(f"Wrote {out}: {len(rows)} books, {dups} look like duplicates.")


def cmd_convert(csv_path, out_dir):
    csv_path = os.path.expanduser(csv_path)
    out_dir = os.path.abspath(os.path.expanduser(out_dir))
    os.makedirs(out_dir, exist_ok=True)
    with open(csv_path, newline="", encoding="utf-8-sig") as f:
        rows = [r for r in csv.DictReader(f) if (r.get("keep") or "").strip()]
    if not rows:
        sys.exit("No rows have anything in the 'keep' column.")
    convert_tool = tool("ebook-convert")
    done = failed = skipped = 0
    for i, r in enumerate(rows, 1):
        src = r["file"]
        name = re.sub(r'[/:\\*?"<>|]+', " ", f"{r['title']} - {r['author']}".strip(" -"))[:150] + ".epub"
        dst = os.path.join(out_dir, name)
        print(f"[{i}/{len(rows)}] {name}")
        if os.path.exists(dst):
            skipped += 1
            continue
        if not os.path.exists(src):
            print("    missing: " + src)
            failed += 1
            continue
        if src.lower().endswith(".epub"):
            shutil.copy2(src, dst)
            done += 1
            continue
        p = subprocess.run([convert_tool, src, dst], capture_output=True, text=True)
        if p.returncode == 0 and os.path.exists(dst):
            done += 1
        else:
            failed += 1
            why = "DRM (copy protection)" if "DRM" in (p.stdout + p.stderr) else "see Calibre's message"
            print(f"    failed: {why}")
    print(f"\n{done} converted, {skipped} already there, {failed} failed. EPUBs are in {out_dir}")


def main(argv):
    if len(argv) == 3 and argv[1] == "list":
        cmd_list(argv[2])
    elif len(argv) == 4 and argv[1] == "convert":
        cmd_convert(argv[2], argv[3])
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main(sys.argv)
