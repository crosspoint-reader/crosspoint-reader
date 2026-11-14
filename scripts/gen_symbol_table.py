"""
PlatformIO post-build script: store the firmware's function names inside
firmware.bin, so a crash report can name code addresses without the ELF.

The table is added to the app image as one more segment with load address 0.
The bootloader treats that address as a padding block: it hashes the data but
never loads or maps it, so the table costs flash only. The file stays a valid
image (segment count, checksum and SHA-256 are rewritten), which is all that
OTA, SD-card update and USB flashing check. lib/hal/SymbolTable.cpp reads it. firmware.factory.bin is merged earlier in
the build and does not carry the table.

Only code that a CrossPoint developer can act on keeps one name per function.
The rest is stored as ranges named after the library, such as "[libnet80211]".
The linker map says which library owns an address, and two lists decide:
  - sources and libraries built by the project keep their names by default;
    COLLAPSED_LIBRARIES lists the exceptions (TLS, BLE, network libraries);
  - prebuilt archives (ESP-IDF, radio stacks, toolchain) and the ROM are
    collapsed by default; NAMED_ARCHIVES lists the core runtime that is kept.
This policy is what keeps the table small: a large new dependency grows the
table unless it is added to COLLAPSED_LIBRARIES.

Table format (little endian):

  0   "ESYM", u16 version (1), u16 header size (16)
  8   u32 block count, u32 largest inflated block size
  16  index: (block count + 1) entries {u32 address, u32 offset}.
      Block i holds the entries from its address up to the next index
      entry's address; its data is at table offset `offset` and ends at the
      next index entry's offset. The last index entry closes the last block.
  then the blocks: raw deflate streams. An inflated block is a list of
      entries in ascending order:
        varint  start address, as the distance from the previous entry's
                start (from the block's address for the first one)
        name, NUL-terminated (function names are cut to 79 characters)
      An entry covers the addresses up to the next entry. An empty name
      marks addresses that belong to no function.

Varints are LEB128. C++ names are demangled and stored without template
arguments, parameters and return type.

Command line, for inspection:
  gen_symbol_table.py add <firmware.elf> <firmware.map> <firmware.bin> --nm <nm>
  gen_symbol_table.py lookup <firmware.bin> <hex address>...
"""

import bisect
import hashlib
import os
import re
import struct
import subprocess
import sys
import zlib

MAGIC = b"ESYM"
VERSION = 1
HEADER_SIZE = 16
NAME_MAX = 79
# Inflated size at which a block is closed. The firmware allocates one block.
BLOCK_SIZE = 8192
# ESP-IDF and toolchain archives whose functions keep their own names.
NAMED_ARCHIVES = {
    "libc", "libcxx", "libfreertos", "libFrameworkArduino", "libgcc", "libheap",
    "libesp_system", "libnewlib", "libpthread", "libriscv", "libstdc++", "libxtensa",
}
# Project library dependencies stored as one range per library.
COLLAPSED_LIBRARIES = {
    "Arduino-wolfSSL", "AsyncUDP", "DNSServer", "ESPmDNS", "Network", "NimBLE-Arduino",
    "WebServer", "WebSockets", "WiFi",
}
# A hole between functions smaller than this is alignment padding.
MIN_GAP = 16
# Mask ROM code of the ESP32-C3 and ESP32-S3, stored as one "[ROM]" range. It lies below all other code.
ROM_RANGE = (0x40000000, 0x40060000)
# A symbol without a size (assembly) is kept only when the next symbol is this close.
UNSIZED_SPAN = 0x1000

IMAGE_MAGIC = 0xE9
IMAGE_HEADER_SIZE = 24
IMAGE_MAX_SEGMENTS = 16
CHECKSUM_SEED = 0xEF
SHA_SIZE = 32


OPERATOR_SYMBOL = re.compile(r"operator\s*(<<=|>>=|<=>|<<|>>|<=|>=|->\*|->|<|>|\(\))")
LAMBDA_BODY = re.compile(r"\{lambda[^{}]*\}::(operator\(\)|_FUN)")
LAMBDA_SUFFIX = "::{lambda}"
NOT_A_RETURN_TYPE = ("non-virtual thunk", "virtual thunk", "guard variable")


def short_name(name):
    """A demangled name without template arguments, parameters and return type."""
    out = []
    depth = 0
    i = 0
    while i < len(name):
        operator = OPERATOR_SYMBOL.match(name, i) if depth == 0 else None
        if operator:
            out.append(operator.group())
            i = operator.end()
            continue
        c = name[i]
        if c == "<":
            if depth == 0:
                out.append("<>")
            depth += 1
        elif c == ">":
            depth -= 1
        elif depth == 0:
            out.append(c)
        i += 1
    name = "".join(out).replace("(anonymous namespace)", "{anon}")
    # A lambda's own code is its call operator. Lambda types also appear in template
    # arguments (gone by now) and in parameter lists, which do not make a lambda body.
    is_lambda = LAMBDA_BODY.search(name) is not None

    start = 0
    while True:
        paren = name.find("(", start)
        if paren <= 0:
            break
        if name.endswith("operator", 0, paren):
            start = paren + 2
            continue
        name = name[:paren]
        break

    # "Item& std::vector<>::emplace_back<>": the return type ends at the last space
    # that is not part of a conversion operator ("operator bool").
    operator = name.find("operator")
    space = name.rfind(" ", 0, operator if operator >= 0 else len(name))
    if space >= 0 and not name.startswith(NOT_A_RETURN_TYPE):
        name = name[space + 1:]
    # A lambda body is its own function: without this it would carry its parent's name.
    if is_lambda and name:
        return name[:NAME_MAX - len(LAMBDA_SUFFIX)] + LAMBDA_SUFFIX
    return name[:NAME_MAX]


def elf_functions(nm, elf):
    """[(address, size, name)] sorted by address, one name per address."""
    out = subprocess.run([nm, "-S", "-C", "--defined-only", elf], check=True,
                         capture_output=True, text=True).stdout
    best = {}
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4 and len(parts[2]) == 1:
            addr, size, kind, name = parts
            size = int(size, 16)
        else:
            parts = line.split(None, 2)
            if len(parts) != 3:
                continue
            addr, kind, name = parts
            size = 0
        addr = int(addr, 16)
        if kind not in "tTwW":
            continue
        name = short_name(name) or name[:NAME_MAX]
        if not name or name.startswith((".L", "$")):
            continue
        # Aliases: prefer a sized symbol, then no leading underscore, then the shortest.
        rank = (size == 0, name.startswith("_"), len(name), name)
        if addr not in best or rank < best[addr][0]:
            best[addr] = (rank, size, name)
    funcs = []
    for addr in sorted(best):
        _, size, name = best[addr]
        if funcs and not size and addr < funcs[-1][0] + funcs[-1][1]:
            continue  # a label inside a function: the function's name is the useful one
        funcs.append((addr, size, name))
    return funcs


MAP_INPUT_SECTION = re.compile(r"^ \S*\s+0x([0-9a-f]{8,16})\s+0x([0-9a-f]+)\s+(\S+\.o(?:bj)?\)?)$")
PROJECT_LIBRARY = re.compile(r"/lib[0-9a-f]{3}/([^/]+)/")
ARCHIVE = re.compile(r"([^/]+)\.a\(")


class CodeOwners:
    """Which library each code address was linked from, read from the linker map."""

    def __init__(self, map_path):
        if not os.path.exists(map_path):
            raise ValueError("%s is missing: delete the .elf next to it to link again" % map_path)
        ranges = []
        with open(map_path, errors="replace") as lines:
            for line in lines:
                match = MAP_INPUT_SECTION.match(line)
                if match and int(match.group(2), 16):
                    ranges.append((int(match.group(1), 16), int(match.group(2), 16), match.group(3)))
        libraries = {m.group(1) for m in (PROJECT_LIBRARY.search(path) for _, _, path in ranges) if m}
        # Without these the policy would be applied to nothing, and the build would still pass.
        if not libraries:
            raise ValueError("no project library found in %s: its layout is not what this expects" % map_path)
        for name in sorted(COLLAPSED_LIBRARIES - libraries):
            print("WARNING [gen_symbol_table.py]: %s is in COLLAPSED_LIBRARIES but not in this build" % name,
                  file=sys.stderr)
        ranges.sort()
        self.starts = [start for start, _, _ in ranges]
        self.ranges = ranges

    def collapsed_library(self, addr):
        """The library name to store in place of the function at addr, or None to keep its name."""
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0 or addr >= self.ranges[i][0] + self.ranges[i][1]:
            return None
        path = self.ranges[i][2]
        library = PROJECT_LIBRARY.search(path)
        if library:
            return library.group(1) if library.group(1) in COLLAPSED_LIBRARIES else None
        archive = ARCHIVE.search(path)
        if not archive or archive.group(1) in NAMED_ARCHIVES:
            return None
        return archive.group(1)


def table_entries(funcs, owners):
    """[(address, name)] in ascending order; an empty name ends the code before a hole."""
    if funcs[0][0] < ROM_RANGE[1]:
        raise ValueError("function %s at 0x%08x is inside the ROM range" % (funcs[0][2], funcs[0][0]))
    entries = [(ROM_RANGE[0], "[ROM]")]
    end = ROM_RANGE[1]
    for i, (addr, size, name) in enumerate(funcs):
        if not size:
            # Runs to the next symbol. With none close by it is a marker such as
            # _etext, and the addresses after it are not code.
            if i + 1 == len(funcs) or funcs[i + 1][0] - addr > UNSIZED_SPAN:
                continue
            size = funcs[i + 1][0] - addr
        library = owners.collapsed_library(addr)
        if library:
            name = "[%s]" % library
        if addr - end >= MIN_GAP:
            entries.append((end, ""))
        # One entry covers a run of functions from the same collapsed library.
        if not (library and entries and entries[-1][1] == name):
            entries.append((addr, name))
        end = addr + size
    entries.append((end, ""))
    return entries


def varint(value):
    out = bytearray()
    while value >= 0x80:
        out.append(value & 0x7F | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def deflate(data):
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    return compressor.compress(data) + compressor.flush()


def build_table(entries):
    blocks = []  # (address, inflated data)
    block = bytearray()
    block_address = previous = 0
    # The last entry is the end marker: it closes the last block in the index.
    for addr, name in entries[:-1]:
        if not block:
            block_address = previous = addr
        block += varint(addr - previous) + name.encode("utf-8", "replace") + b"\0"
        previous = addr
        if len(block) >= BLOCK_SIZE:
            blocks.append((block_address, bytes(block)))
            block = bytearray()
    if block:
        blocks.append((block_address, bytes(block)))

    index = bytearray()
    body = bytearray()
    body_offset = HEADER_SIZE + (len(blocks) + 1) * 8
    for address, data in blocks:
        index += struct.pack("<II", address, body_offset + len(body))
        body += deflate(data)
    index += struct.pack("<II", entries[-1][0], body_offset + len(body))
    header = MAGIC + struct.pack("<HHII", VERSION, HEADER_SIZE, len(blocks), max(len(d) for _, d in blocks))
    return header + bytes(index) + bytes(body)


def parse_image(data):
    """The segments of an ESP app image: [(load address, data)]."""
    if len(data) < IMAGE_HEADER_SIZE or data[0] != IMAGE_MAGIC:
        raise ValueError("not an ESP app image")
    segments = []
    pos = IMAGE_HEADER_SIZE
    for _ in range(data[1]):
        if pos + 8 > len(data):
            raise ValueError("truncated image")
        addr, size = struct.unpack_from("<II", data, pos)
        pos += 8
        if pos + size > len(data):
            raise ValueError("truncated image")
        segments.append((addr, bytes(data[pos:pos + size])))
        pos += size
    return segments


def is_table(segment):
    return segment[0] == 0 and segment[1][:4] == MAGIC


def write_image(data, segments):
    """The image of `data` with these segments, checksum and SHA-256 redone."""
    if len(segments) > IMAGE_MAX_SEGMENTS:
        raise ValueError("no free segment slot in the image")
    out = bytearray(data[:IMAGE_HEADER_SIZE])
    out[1] = len(segments)
    checksum = CHECKSUM_SEED
    for addr, seg in segments:
        out += struct.pack("<II", addr, len(seg)) + seg
        # XOR of all bytes, folded 8 bytes at a time
        acc = 0
        for (word,) in struct.iter_unpack("<Q", seg + bytes(-len(seg) % 8)):
            acc ^= word
        for shift in range(0, 64, 8):
            checksum ^= (acc >> shift) & 0xFF
    out += bytes(15 - len(out) % 16)
    out.append(checksum)
    if data[23]:  # hash_appended
        out += hashlib.sha256(out).digest()
    return bytes(out)


def add_table(nm, elf, map_path, binfile, max_size=0):
    """max_size: the app partition size, or 0 when not known."""
    data = open(binfile, "rb").read()
    segments = parse_image(data)
    # Rewriting the unchanged image must reproduce the file, or the layout is not what this expects.
    if write_image(data, segments) != data:
        raise ValueError("image layout not understood; table not added")
    segments = [s for s in segments if not is_table(s)]
    funcs = elf_functions(nm, elf)
    entries = table_entries(funcs, CodeOwners(map_path))
    table = build_table(entries)
    table += bytes(-len(table) % 4)
    image = write_image(data, segments + [(0, table)])
    # PlatformIO checked the size before the table existed.
    if max_size and len(image) > max_size:
        raise ValueError("firmware with symbol table is %d bytes, the app partition holds %d"
                         % (len(image), max_size))
    with open(binfile, "wb") as f:
        f.write(image)
    blocks, block_size = struct.unpack_from("<II", table, 8)
    named = sum(1 for _, name in entries if name and not name.startswith("["))
    print("Symbol table: %d of %d functions named, %d blocks of up to %d bytes, %.1f KB; "
          "image %.1f KB -> %.1f KB%s"
          % (named, len(funcs), blocks, block_size, len(table) / 1024, len(data) / 1024, len(image) / 1024,
             " of %.1f KB" % (max_size / 1024) if max_size else ""))


def find_table(data):
    for segment in parse_image(data):
        if is_table(segment):
            return segment[1]
    raise ValueError("no symbol table")


def read_varint(data, pos):
    value = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return value, pos


def lookup(table, addr):
    """Mirrors SymbolTable::lookup in lib/hal/SymbolTable.cpp."""
    blocks = struct.unpack_from("<I", table, 8)[0]
    index = [struct.unpack_from("<II", table, HEADER_SIZE + i * 8) for i in range(blocks + 1)]
    if not index[0][0] <= addr < index[-1][0]:
        return None
    i = max(k for k in range(blocks) if index[k][0] <= addr)
    block = zlib.decompress(table[index[i][1]:index[i + 1][1]], -15)
    start = index[i][0]
    pos = 0
    found = None
    while pos < len(block):
        delta, pos = read_varint(block, pos)
        end = block.index(b"\0", pos)
        start += delta
        if addr < start:
            break
        found = (block[pos:end].decode("utf-8", "replace"), start)
        pos = end + 1
    if not found or not found[0]:
        return None
    return "%s+0x%x" % (found[0], addr - found[1])


def main(argv):
    if len(argv) == 6 and argv[0] == "add" and argv[4] == "--nm":
        add_table(argv[5], argv[1], argv[2], argv[3])
    elif len(argv) >= 3 and argv[0] == "lookup":
        table = find_table(open(argv[1], "rb").read())
        for arg in argv[2:]:
            print("0x%08x %s" % (int(arg, 16), lookup(table, int(arg, 16)) or "?"))
    else:
        sys.exit(__doc__)


try:
    Import("env")  # noqa: F821 (SCons-injected global)
except NameError:
    if __name__ == "__main__":
        main(sys.argv[1:])
else:
    def _add_table_action(source, target, env):  # pylint: disable=unused-argument
        cc = env.subst("$CC")
        if not cc.endswith("gcc"):
            raise ValueError("cannot derive nm from compiler %r" % cc)
        add_table(cc[:-3] + "nm", env.subst("$BUILD_DIR/${PROGNAME}.elf"),
                  env.subst("$BUILD_DIR/${PROGNAME}.map"), str(target[0]),
                  int(env.BoardConfig().get("upload.maximum_size", 0)))

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _add_table_action)  # noqa: F821
    # The linker writes the map: an ELF taken from the build cache would come without it.
    env.NoCache("$BUILD_DIR/${PROGNAME}.elf")  # noqa: F821
    # Without this, an edit to this script would keep using the image in the build cache.
    env.Depends("$BUILD_DIR/${PROGNAME}.bin", "$PROJECT_DIR/scripts/gen_symbol_table.py")  # noqa: F821
