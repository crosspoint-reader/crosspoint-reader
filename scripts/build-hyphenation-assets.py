#!/usr/bin/env python3
"""Build versioned CrossPoint hyphenation packs from a pinned Hypher checkout."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import subprocess
import zipfile
import zlib
from pathlib import Path


PACK_HEADER = struct.Struct("<4sB2sBBB2xIII")
PACK_VERSION = 1
LANGUAGE_RE = re.compile(
    r"/// Hyphenation for _(.+?)\._ \(Code: `([a-z]{2})`,.*?Script, `([A-Za-z]{4})`, "
    r"Feature: `([^`]+)`\)\s*#\[cfg\(feature = \"[^\"]+\"\)\]\s*(\w+),"
)
BOUNDS_RE = re.compile(r"Self::(\w+) => \((\d+), (\d+)\),")


def build_assets(upstream: Path, output: Path, base_url: str, revision: str) -> None:
    actual_revision = subprocess.check_output(
        ["git", "-C", str(upstream), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual_revision != revision:
        raise ValueError(f"Hypher checkout is {actual_revision}, expected {revision}")

    lang_source = (upstream / "src/lang.rs").read_text()
    languages = LANGUAGE_RE.findall(lang_source.split("impl Lang", 1)[0])
    if len(languages) != 48:
        raise ValueError(f"Expected 48 Hypher languages, found {len(languages)}")
    bounds = {name: (int(left), int(right)) for name, left, right in BOUNDS_RE.findall(lang_source)}
    no_hyphen_source = lang_source.split("pub fn hyphenation_character", 1)[1].split("fn root", 1)[0]
    no_hyphen = set(re.findall(r"Self::(\w+) => None", no_hyphen_source))

    output.mkdir(parents=True, exist_ok=True)
    catalog = []
    hashes = []
    for name, code, script, _feature, variant in sorted(languages, key=lambda item: item[1]):
        raw = (upstream / "tries" / f"{code}.bin").read_bytes()
        if len(raw) < 5:
            raise ValueError(f"{code}: empty or truncated trie")
        root_offset = int.from_bytes(raw[:4], "big") - 4
        payload = raw[4:]
        if not 0 <= root_offset < len(payload):
            raise ValueError(f"{code}: invalid root offset")
        if variant not in bounds:
            raise ValueError(f"{code}: missing language bounds")
        left, right = bounds[variant]
        flags = int(variant in no_hyphen)
        crc32 = zlib.crc32(payload)
        package = PACK_HEADER.pack(
            b"CPHY", PACK_VERSION, code.encode("ascii"), left, right, flags,
            root_offset, len(payload), crc32,
        ) + payload
        filename = f"hyph-{code}.cphyph"
        (output / filename).write_bytes(package)
        hashes.append(f"{hashlib.sha256(package).hexdigest()}  {filename}")
        catalog.append({
            "code": code,
            "name": name,
            "script": script,
            "file": filename,
            "size": len(package),
            "crc32": zlib.crc32(package),
            "payloadCrc32": crc32,
        })

    manifest = {
        "version": PACK_VERSION,
        "source": {"repository": "typst/hypher", "revision": revision},
        "baseUrl": base_url.rstrip("/") + "/",
        "packs": catalog,
    }
    (output / "hyphenation.json").write_text(json.dumps(manifest, separators=(",", ":")) + "\n")
    hashes.append(f"{hashlib.sha256((output / 'hyphenation.json').read_bytes()).hexdigest()}  hyphenation.json")
    source_archive = output / "pattern-sources.zip"
    with zipfile.ZipFile(source_archive, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for source in sorted((upstream / "patterns").iterdir()):
            if source.is_file():
                info = zipfile.ZipInfo(f"patterns/{source.name}", (1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, source.read_bytes())
        for license_name in ("README.md", "LICENSE-APACHE", "LICENSE-MIT"):
            info = zipfile.ZipInfo(license_name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, (upstream / license_name).read_bytes())

    hashes.append(f"{hashlib.sha256(source_archive.read_bytes()).hexdigest()}  {source_archive.name}")
    (output / "SHA256SUMS").write_text("\n".join(hashes) + "\n")

    print(f"Built {len(catalog)} packs from Hypher {revision}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hypher", type=Path, required=True, help="Pinned Hypher checkout")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--base-url", required=True, help="GitHub Release asset URL prefix")
    parser.add_argument("--revision", required=True, help="Expected Hypher commit")
    args = parser.parse_args()
    build_assets(args.hypher, args.output, args.base_url, args.revision)
