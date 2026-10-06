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
    python3 booklist.py prune books.csv        -> show the books NOT marked "keep"
    python3 booklist.py prune books.csv --apply   ...and move them to the Trash
    python3 booklist.py tidy books.csv         -> show title/author fixes
    python3 booklist.py tidy books.csv --apply    ...and write them into the books

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

`prune` moves every file whose "keep" cell is empty to the Trash (so it can
be put back from there). `tidy` fixes each kept book's own title and author
(what the reader shows): Title Case ("The demolished man" -> "The Demolished
Man", "The Letter Of Marque" -> "The Letter of Marque"), drops a bare
"a novel" subtitle, turns "Joe Pitt 1 - Already Dead" into the title
"Already Dead" in the series "Joe Pitt", and mends "Mccarthy". Words that
already carry deliberate capitals (McCarthy, UR, H.M.S., iPhone) are left
alone. Both only print what they would do until run again with --apply.
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


# Articles, short conjunctions and prepositions: lower case inside a title.
# Not "up", "off" or "than": "Looking Up" and "Less Than Zero" are how
# those books spell themselves.
SMALL_WORDS = {
    "a", "an", "the", "and", "but", "or", "nor", "for", "as", "at", "by", "in", "of",
    "on", "to", "via", "vs", "with", "from", "into", "onto", "upon",
}
WORD = re.compile(r"[^\s]+")


def _cap(token):
    """First letter up, the rest as written (cuckoo's -> Cuckoo's)."""
    for i, c in enumerate(token):
        if c.isalpha():
            return token[:i] + c.upper() + token[i + 1:]
    return token


def _core(token):
    return re.sub(r"^[^\w]+|[^\w]+$", "", token)


def title_case(title):
    """Chicago-style Title Case that leaves deliberate capitals alone."""
    text = " ".join(title.split())
    if text.isupper() and len(text) > 4:
        text = text.lower()  # A TITLE IN CAPITALS
    tokens = WORD.findall(text)
    out = []
    for i, tok in enumerate(tokens):
        core = _core(tok)
        first = i == 0 or out[-1].endswith((":", ".", "?", "!", "-", "\u2014")) or tok[:1] in "(\"'\u201c\u2018"
        # "Natural Selection, or, The Preservation": an alternative title
        # after "or," keeps the capital it was given.
        if i > 0 and out[-1].endswith(",") and core[:1].isupper():
            first = True
        last = i == len(tokens) - 1
        parts = tok.split("-")
        fixed = []
        for j, part in enumerate(parts):
            pc = _core(part)
            if not pc or any(ch.isdigit() for ch in pc):
                fixed.append(part)
            elif pc != pc.lower() and pc != pc.capitalize():
                fixed.append(part)  # McCarthy, UR, H.M.S., iPhone: on purpose
            elif pc.lower() in SMALL_WORDS and not (first and j == 0) and not last and len(parts) == 1:
                fixed.append(part.lower())
            elif re.fullmatch(r"[ivxlc]+", pc.lower()) and pc.lower() in ("ii", "iii", "iv", "vi", "vii", "viii", "ix", "xi", "xii"):
                fixed.append(part.upper())
            else:
                fixed.append(_cap(part))
        out.append("-".join(fixed))
    return " ".join(out)


def tidy_title(title):
    """(title, series, index) with Title Case, no bare "a novel", series split off."""
    t = " ".join(title.split())
    t = re.sub(r"\s*[:;,]\s*a\s+(?:[\w.]+\s+)?novel\s*$", "", t, flags=re.I)  # ": a novel", ": A Reacher Novel"
    t = re.sub(r"\s+a novel$", "", t, flags=re.I)
    series, index = "", ""
    m = re.match(r"^(.+?)\s+(\d{1,2})\s+-\s+(.+)$", t)  # "Joe Pitt 1 - Already Dead"
    if m:
        series, index, t = m.group(1), m.group(2), m.group(3)
    m = re.match(r"^(A Series of Unfortunate Events),\s+(.+)$", t, flags=re.I)
    if m:
        series, t = m.group(1), m.group(2)
    return title_case(t), title_case(series) if series else "", index


def tidy_author(author):
    """"Cormac Mccarthy" -> McCarthy, lower-case names capitalised, & between authors."""
    names = [n.strip() for n in author.split(";") if n.strip()]
    fixed = []
    for n in names:
        if n == n.lower() or n == n.upper():
            n = re.sub(r"(^|[\s.\-'])([a-z])", lambda m: m.group(1) + m.group(2).upper(), n.lower())
        n = re.sub(r"\bMc([a-z])", lambda m: "Mc" + m.group(1).upper(), n)
        fixed.append(n)
    return " & ".join(fixed)


def read_rows(csv_path):
    with open(os.path.expanduser(csv_path), newline="", encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))


def cmd_prune(csv_path, apply):
    rows = read_rows(csv_path)
    gone = [r for r in rows if not (r.get("keep") or "").strip() and os.path.exists(r["file"])]
    kept = sum(1 for r in rows if (r.get("keep") or "").strip())
    if not gone:
        sys.exit("Nothing to remove: every book that is still there is marked keep.")
    size = sum(os.path.getsize(r["file"]) for r in gone) / 1e6
    for r in gone:
        print(f"  {r['author']} - {r['title']}")
    print(f"\n{len(gone)} books ({size:.0f} MB) are not marked keep; {kept} are.")
    if not apply:
        print("Nothing was moved. Run the same command with --apply at the end to move them to the Trash.")
        return
    trash = os.path.expanduser("~/.Trash")
    os.makedirs(trash, exist_ok=True)
    moved = 0
    for r in gone:
        dst = os.path.join(trash, os.path.basename(r["file"]))
        stem, ext = os.path.splitext(dst)
        n = 2
        while os.path.exists(dst):
            dst = f"{stem} {n}{ext}"
            n += 1
        shutil.move(r["file"], dst)
        moved += 1
    print(f"Moved {moved} books to the Trash. To undo: open the Trash, select them, File > Put Back.")


def cmd_tidy(csv_path, apply):
    rows = [r for r in read_rows(csv_path) if (r.get("keep") or "").strip() and os.path.exists(r["file"])]
    changes = []
    for r in rows:
        title, series, index = tidy_title(r["title"])
        author = tidy_author(r["author"])
        if title != r["title"] or author != r["author"].replace(";", " & ").replace("  ", " ") or series:
            changes.append((r, title, author, series, index))
    if not changes:
        print("Every kept book's title and author already look right.")
        return
    for r, title, author, series, index in changes:
        line = f"  {r['title']}  ->  {title}"
        if series:
            line += f"   [series: {series}{' #' + index if index else ''}]"
        if author != r["author"]:
            line += f"   (author: {author})"
        print(line)
    print(f"\n{len(changes)} of {len(rows)} kept books would change.")
    if not apply:
        print("Nothing was changed. Run the same command with --apply at the end to write these into the books.")
        return
    meta_tool = tool("ebook-meta")
    failed = 0
    for i, (r, title, author, series, index) in enumerate(changes, 1):
        args = [meta_tool, r["file"], "-t", title, "-a", author]
        if series:
            args += ["--series", series]
            if index:
                args += ["--index", index]
        p = subprocess.run(args, capture_output=True, text=True)
        if p.returncode != 0:
            failed += 1
            print(f"    failed: {r['file']}")
        if i % 25 == 0 or i == len(changes):
            print(f"  {i}/{len(changes)}")
    print(f"Done: {len(changes) - failed} books updated, {failed} failed.")


def main(argv):
    apply = "--apply" in argv
    argv = [a for a in argv if a != "--apply"]
    if len(argv) == 3 and argv[1] == "list":
        cmd_list(argv[2])
    elif len(argv) == 4 and argv[1] == "convert":
        cmd_convert(argv[2], argv[3])
    elif len(argv) == 3 and argv[1] == "prune":
        cmd_prune(argv[2], apply)
    elif len(argv) == 3 and argv[1] == "tidy":
        cmd_tidy(argv[2], apply)
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main(sys.argv)
