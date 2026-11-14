"""
Builds the images SymbolTableTest reads, with the firmware's own generator
(scripts/gen_symbol_table.py), so a change to the table format on either side
fails the test.

  make_fixture.py <gen_symbol_table.py> <output directory>

ENTRIES is mirrored by the expectations in SymbolTableTest.cpp: change both.
"""

import importlib.util
import os
import sys

# (start address, name). An empty name marks addresses that belong to no function.
# The last entry is the end of the table.
ENTRIES = [
    (0x40000000, "[ROM]"),
    (0x40060000, ""),
    (0x42000000, "alpha"),
    (0x42000010, "beta::gamma"),
    (0x42000030, ""),
    (0x42000100, "[libnet80211]"),
    (0x42000400, "Reader::render::{lambda}"),
    (0x42000420, "std::vector<>::push_back"),
    (0x42000500, "epsilon"),
    (0x42000502, "zeta"),
    (0x42010000, "far_away"),
    (0x42010040, ""),
]
# Small enough to spread ENTRIES over several blocks.
BLOCK_SIZE = 32


def main(script_path, out_dir):
    spec = importlib.util.spec_from_file_location("gen_symbol_table", script_path)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    generator.BLOCK_SIZE = BLOCK_SIZE

    table = generator.build_table(ENTRIES)
    table += bytes(-len(table) % 4)

    # A minimal app image: one ordinary segment, SHA-256 appended.
    header = bytearray(generator.IMAGE_HEADER_SIZE)
    header[0] = generator.IMAGE_MAGIC
    header[23] = 1
    code = (0x3C000020, bytes(range(32)))

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "with_table.bin"), "wb") as f:
        f.write(generator.write_image(bytes(header), [code, (0, table)]))
    with open(os.path.join(out_dir, "without_table.bin"), "wb") as f:
        f.write(generator.write_image(bytes(header), [code, (0, bytes(64))]))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
