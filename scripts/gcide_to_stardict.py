#!/usr/bin/env python3
"""Convert GCIDE (CIDE.A..CIDE.Z) into a StarDict dictionary for CrossPoint.

Usage: python3 scripts/gcide_to_stardict.py <gcide-dir> <out-dir> [stem]

<gcide-dir> is an unpacked GCIDE release (e.g. gcide-0.54 from
https://ftp.gnu.org/gnu/gcide/). Writes <out-dir>/<stem>.ifo, .idx and .dict
(uncompressed, 32-bit offsets, sametypesequence=h HTML definitions). The .idx
is sorted the way the reader searches it: ASCII case-insensitive, then by raw
bytes. Copy <out-dir> to /dictionaries/<stem>/ on the SD card.
"""
import html
import os
import re
import struct
import sys
import unicodedata

COMBINING = {
    "acute": "\u0301", "grave": "\u0300", "cir": "\u0302", "til": "\u0303",
    "mac": "\u0304", "cr": "\u0306", "dot": "\u0307", "um": "\u0308",
    "ring": "\u030A", "car": "\u030C", "sdot": "\u0323", "sl": "\u0304",
    "ced": "\u0327",
}


def load_entities(gcide_dir):
    """Entity name and \\'xx escape -> Unicode text, from webfont.txt."""
    entities, escapes = {}, {}
    row = re.compile(r"^\d{3}\s+\d+\s+([0-9a-f]{2})\s+(?:<([^\s/]+)/)?")
    uplus = re.compile(r"<([^\s/]+)/[^(\n]*\(U\+([0-9A-Fa-f]{4,6})\)")
    with open(os.path.join(gcide_dir, "webfont.txt"), encoding="utf-8", errors="replace") as f:
        for line in f:
            m = row.match(line)
            if m:
                char = next((c for c in line[m.end():] if ord(c) > 127), None)
                if char:
                    escapes[m.group(1)] = char
                    if m.group(2):
                        entities[m.group(2)] = char
                continue
            for name, code in uplus.findall(line):
                value = int(code, 16)
                if value <= 0x10FFFF and not 0xD800 <= value <= 0xDFFF:
                    entities.setdefault(name, chr(value))
    # Common entities the table describes in prose rather than rows.
    entities.update({"ldquo": "\u201c", "rdquo": "\u201d", "lsquo": "\u2018", "rsquo": "\u2019",
                     "mdash": "\u2014", "ndash": "\u2013", "br": "\n", "hand": "\u261e",
                     "root": "\u221a", "deg": "\u00b0", "middot": "\u00b7", "frac12": "\u00bd",
                     "?": "?", "th": "th", "-": "\u2014", "--": "\u2014", "prime": "\u2032",
                     "bprime": "\u2033", "sec": "\u2033", "oomac": "o\u0304o\u0304"})
    return entities, escapes


GREEK = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa", "lambda",
         "mu", "nu", "xi", "omicron", "pi", "rho", "sigma", "tau", "upsilon", "phi", "chi", "psi", "omega"}
WORDS = {"or": "or", "and": "and", "colbreak": "", "times": "\u00d7", "frac34": "\u00be", "rarr": "\u2192",
         "cuberoot": "\u221b", "bar": "|", "min": "\u2032",
         # Vowels with a double dot below (diaeresis below).
         "add": "a\u0324", "udd": "u\u0324", "odd": "o\u0324", "edd": "e\u0324", "idd": "i\u0324"}


def entity_text(name, entities):
    if name in entities:
        return entities[name]
    if name in WORDS:
        return unicodedata.normalize("NFC", WORDS[name])
    if name.lower() in GREEK:
        case = "SMALL" if name.islower() else "CAPITAL"
        letter = "LAMDA" if name.lower() == "lambda" else name.upper()  # Unicode spelling
        return unicodedata.lookup(f"GREEK {case} LETTER {letter}")
    # <xit/ italic vowels, and diacritic codes such as <emac/ or <Ocr/.
    if name.endswith("it") and len(name) == 3:
        return name[0]
    for suffix, mark in sorted(COMBINING.items(), key=lambda kv: -len(kv[0])):
        if name.endswith(suffix) and 0 < len(name) - len(suffix) <= 2:
            return unicodedata.normalize("NFC", name[: -len(suffix)] + mark)
    return ""


ITALIC_TAGS = {"pos", "ex", "it", "qex", "as", "xex", "spn", "col", "cd", "fld", "mark", "plw", "asp",
               "conjf", "altname", "vmorph", "stype", "gen", "plu", "altsp", "mcol", "wf", "cref", "er"}
BOLD_TAGS = {"hw", "sn", "b", "mhw"}
DROP_TAGS = {"source"}


def to_html(raw, entities, escapes):
    """GCIDE markup of one entry -> small HTML fragment."""
    text = re.sub(r"\\'([0-9a-f]{2})", lambda m: "\x00E" + escapes.get(m.group(1), "") + "\x00", raw)
    text = re.sub(r"<([A-Za-z0-9?\-]+)/(?!>)", lambda m: "\x00E" + entity_text(m.group(1), entities) + "\x00", text)
    # Stress and syllable marks in pronunciations: (ab"a*kus) -> (abakus).
    text = re.sub(r"<pr>(.*?)</pr>", lambda m: re.sub(r'[*"`]', "", m.group(1)), text, flags=re.S)
    # Attribution brackets such as [<source>Webster 1913</source>].
    text = re.sub(r"\[\s*(?:<source>.*?</source>[\s,+A-Za-z.]*)+\]", "", text, flags=re.S)
    out = []
    pos = 0
    for m in re.finditer(r"<(/?)([a-zA-Z]+)[^>]*>|\x00E(.*?)\x00", text, flags=re.S):
        out.append(html.escape(text[pos:m.start()], quote=False))
        pos = m.end()
        if m.group(3) is not None:
            ent = m.group(3)
            out.append("<br/>" if ent == "\n" else html.escape(ent, quote=False))
            continue
        closing, tag = m.group(1) == "/", m.group(2).lower()
        if tag in DROP_TAGS:
            continue
        if tag in BOLD_TAGS:
            out.append("</b>" if closing else "<b>")
        elif tag in ITALIC_TAGS:
            out.append("</i>" if closing else "<i>")
        elif tag == "p":
            # The block is one top-level paragraph; a nested one becomes a line break.
            out.append("" if closing else "<br/>")
    out.append(html.escape(text[pos:], quote=False))
    body = "".join(out)
    # Stress and syllable marks inside headwords: Ab"a*cus -> Abacus.
    body = re.sub(r"<b>([^<]*)</b>", lambda m: "<b>" + m.group(1).replace("*", "").replace('"', "").replace("`", "") + "</b>", body)
    body = re.sub(r"\s+", " ", body).replace("<p> ", "<p>")
    body = re.sub(r"^(?:<p>)?(?:\s*<br/>)+\s*", "", body)
    if body and not body.startswith("<p>"):
        body = "<p>" + body
    return body + ("</p>" if body else "")


def headword_text(raw, entities, escapes):
    text = re.sub(r"\\'([0-9a-f]{2})", lambda m: escapes.get(m.group(1), ""), raw)
    text = re.sub(r"<([A-Za-z0-9?\-]+)/(?!>)", lambda m: entity_text(m.group(1), entities), text)
    text = re.sub(r"<[^>]*>", "", text)
    text = re.sub(r'[*"`]', "", text)  # stress and syllable marks, as in headword display
    text = re.sub(r"\s+", " ", text).strip()
    return unicodedata.normalize("NFC", text)


def paragraphs(data):
    """Top-level <p>...</p> bodies; GCIDE nests paragraphs, so track depth."""
    depth, start = 0, 0
    for m in re.finditer(r"<(/?)p>", data):
        if not m.group(1):
            if depth == 0:
                start = m.end()
            depth += 1
        elif depth > 0:
            depth -= 1
            if depth == 0:
                yield data[start:m.start()]


def entries(gcide_dir, entities, escapes):
    """Yields (headwords, html) per GCIDE paragraph group."""
    ent = re.compile(r"<ent>(.*?)</ent>", re.S)
    for letter in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
        path = os.path.join(gcide_dir, "CIDE." + letter)
        with open(path, encoding="latin-1") as f:
            data = f.read().replace("\x00", "")  # NUL would break the index and the entity markers
        heads, parts = [], []
        for block in paragraphs(data):
            names = [headword_text(e, entities, escapes) for e in ent.findall(block)]
            if names:
                if heads and parts:
                    yield heads, "".join(parts)
                heads, parts = [n for n in names if n], []
                block = ent.sub("", block)
            if heads:
                fragment = to_html(block, entities, escapes)
                if fragment and fragment != "<p></p>":
                    parts.append(fragment)
        if heads and parts:
            yield heads, "".join(parts)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    gcide_dir, out_dir = sys.argv[1], sys.argv[2]
    stem = sys.argv[3] if len(sys.argv) > 3 else "gcide"
    os.makedirs(out_dir, exist_ok=True)
    entities, escapes = load_entities(gcide_dir)
    # "gcide-0.54" -> "GCIDE 0.54"; the release folder carries the version.
    release = os.path.basename(os.path.normpath(gcide_dir)).replace("gcide-", "")
    bookname = "GCIDE " + release if release and release != "gcide" else "GCIDE"

    index = []  # (word bytes, offset, size)
    with open(os.path.join(out_dir, stem + ".dict"), "wb") as dict_file:
        offset = 0
        for heads, body in entries(gcide_dir, entities, escapes):
            data = body.encode("utf-8")
            dict_file.write(data)
            seen = set()
            for head in heads:
                word = head.encode("utf-8")
                if not word or len(word) >= 250 or word in seen:
                    continue
                seen.add(word)
                index.append((word, offset, len(data)))
            offset += len(data)
            if offset >= 2**32:
                sys.exit("dictionary exceeds 32-bit offsets")

    index.sort(key=lambda e: (e[0].lower(), e[0]))  # bytes.lower() lowers ASCII only
    with open(os.path.join(out_dir, stem + ".idx"), "wb") as idx_file:
        for word, off, size in index:
            idx_file.write(word + b"\0" + struct.pack(">II", off, size))
    idx_size = os.path.getsize(os.path.join(out_dir, stem + ".idx"))

    with open(os.path.join(out_dir, stem + ".ifo"), "w", encoding="utf-8", newline="\n") as ifo:
        ifo.write("StarDict's dict ifo file\nversion=2.4.2\n")
        ifo.write(f"bookname={bookname}\n")
        ifo.write(f"wordcount={len(index)}\nidxfilesize={idx_size}\nsametypesequence=h\n")
        ifo.write(f"description=GNU Collaborative International Dictionary of English ({bookname}), GPL-3.0+.\n")
    print(f"{len(index)} index entries, .dict {offset} bytes, .idx {idx_size} bytes")


if __name__ == "__main__":
    main()
