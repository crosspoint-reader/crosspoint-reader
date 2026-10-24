#!/usr/bin/env python3
"""Regenerate lib/OtShaper/OtUnicodeData.h.

The shaper classifies characters (general category, combining class, Indic
and USE categories, decompositions, language tags) exactly as HarfBuzz does,
so these tables are read out of HarfBuzz itself: this downloads the pinned
release, compiles gen_ot_shaper_tables.cc against it with the host compiler and writes
the header. HarfBuzz is only needed here, never by the firmware.

    python3 scripts/gen_ot_shaper_tables.py

Set HARFBUZZ_SRC to an unpacked HarfBuzz src/ directory to skip the download.
"""

import hashlib
import os
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

VERSION = "14.5.0"
URL = f"https://github.com/harfbuzz/harfbuzz/releases/download/{VERSION}/harfbuzz-{VERSION}.tar.xz"
SHA256 = "b7132e148358a45185c9feafd049dbaf243649d3c44414b3534d9c95d18592b9"

HERE = Path(__file__).resolve().parent
OUTPUT = HERE.parent / "lib" / "OtShaper" / "OtUnicodeData.h"


def harfbuzz_src(workdir):
    if os.environ.get("HARFBUZZ_SRC"):
        return Path(os.environ["HARFBUZZ_SRC"])
    tarball = Path(workdir) / "harfbuzz.tar.xz"
    urllib.request.urlretrieve(URL, tarball)
    if hashlib.sha256(tarball.read_bytes()).hexdigest() != SHA256:
        sys.exit("HarfBuzz tarball checksum mismatch")
    with tarfile.open(tarball) as tar:
        tar.extractall(workdir, filter="data")
    return Path(workdir) / f"harfbuzz-{VERSION}" / "src"


def main():
    with tempfile.TemporaryDirectory() as workdir:
        src = harfbuzz_src(workdir)
        exe = Path(workdir) / "gen_ot_shaper_tables"
        subprocess.run(["c++", "-std=c++17", "-O1", "-w", f"-I{src}", str(HERE / "gen_ot_shaper_tables.cc"),
                        str(src / "harfbuzz.cc"), "-o", str(exe)], check=True)
        header = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    OUTPUT.write_text(header)
    print(f"wrote {OUTPUT}")


if __name__ == "__main__":
    main()
