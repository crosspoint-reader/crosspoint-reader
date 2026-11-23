"""Compile the private font-ID helper and check persisted identity compatibility."""

import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/SdCardFontSystem.cpp").read_text()
start = source.index("int computeTtfFontId(")
function = source[start:source.index("\n}\n", start) + 3]


def expected_id(family, content_key, point_size, ui):
    data = (
        b"TTF-content-v1\0"
        + (family or "").encode("utf-8") + b"\0"
        + content_key.to_bytes(4, "big")
        + bytes((point_size, ui))
    )
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    signed = value if value < 0x80000000 else value - 0x100000000
    return signed or 1


checks = []
for family in (None, "", "Family", "Other", "Fam\u00edly", "\u5bb6\u65cf"):
    literal = "nullptr" if family is None else json.dumps(family, ensure_ascii=True)
    for content_key in (0, 1, 0x100, 0x10000, 0x1000000, 0x01020304, 0xFFFFFFFF):
        for point_size in (0, 8, 12, 14, 16, 255):
            for ui in (False, True):
                expected = expected_id(family, content_key, point_size, ui)
                checks.append(
                    f"assert(computeTtfFontId({literal}, {content_key}u, {point_size}, "
                    f"{str(ui).lower()}) == {expected});"
                )

harness = (
    "#include <Fnv1a.h>\n#include <cassert>\n#include <cstring>\n"
    + function
    + "\nint main() {\n"
    + "\n".join(checks)
    + '\nassert(computeTtfFontId("Family", 123, 14) == '
      'computeTtfFontId("Family", 123, 14, false));\n}\n'
)

with tempfile.TemporaryDirectory(prefix="vector-font-identity-") as directory:
    cpp = Path(directory) / "identity.cpp"
    executable = Path(directory) / "identity"
    cpp.write_text(harness)
    subprocess.run(
        [os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
         *shlex.split(os.environ.get("CXXFLAGS", "")),
         "-UNDEBUG", "-I", str(ROOT / "lib/EpdFont"), str(cpp), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)

print(f"Vector font identity regression checks passed ({len(checks)} cases)")
