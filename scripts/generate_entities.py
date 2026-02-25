#!/usr/bin/env python3
"""
generate_entities.py
--------------------
Reads htmlEntities.yaml and generates htmlEntities.cpp with a perfect hash table.

A "perfect hash" means: for our fixed, known set of keys, we find hash parameters
that produce ZERO collisions. This gives O(1) lookup: hash -> slot -> verify key.

HOW THE TWO-LEVEL HASH WORKS
-----------------------------
We use the classic "Hash, Displace and Compress" algorithm:

  Level 1 — assign each key to a bucket:
      bucket = poly_hash(key, PRIMARY_SALT) % TABLE_SIZE

  Level 2 — for each bucket, find a displacement value that places all
  of the bucket's keys into empty slots in the final table:
      slot = (poly_hash(key, SECONDARY_SALT) + displacement[bucket]) % TABLE_SIZE

At runtime, lookup is:
    1. Compute bucket  = poly_hash(key, PRIMARY_SALT)   % TABLE_SIZE
    2. Look up         displacement = DISPLACEMENTS[bucket]
    3. Compute slot    = (poly_hash(key, SECONDARY_SALT) + displacement) % TABLE_SIZE
    4. Compare len against HASH_TABLE[slot].keyLen  (precomputed)
    5. memcmp HASH_TABLE[slot].key — if match, return value; else return nullptr


HOW TO RUN
----------
Standalone (default salts):
    python3 scripts/generate_entities.py

Standalone (custom salts — useful for when adding many new entities):
    python3 scripts/generate_entities.py --primary-salt 41 --secondary-salt 29

PlatformIO (add to platformio.ini):
    extra_scripts = pre:scripts/generate_entities.py
"""

import argparse
import sys
import os
import re

# ---------------------------------------------------------------------------
# Paths (relative to project root)
# ---------------------------------------------------------------------------
SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
YAML_PATH    = os.path.join(PROJECT_ROOT, "lib", "Epub", "Epub", "Entities", "htmlEntities.yaml")
OUTPUT_PATH  = os.path.join(PROJECT_ROOT, "lib", "Epub", "Epub", "Entities", "htmlEntities.cpp")

# ---------------------------------------------------------------------------
# Default hash parameters
#
# PRIMARY_SALT: used to assign keys to buckets in the first pass.
#   Value 37 was found to produce well-spread buckets for our entity set.
#   If you add many new entities and placement fails, try a different value
#   via --primary-salt.
#
# SECONDARY_SALT: fixed polynomial base used when placing keys into the
#   final table. 31 is a classic choice (used in Java's String.hashCode).
# ---------------------------------------------------------------------------
DEFAULT_PRIMARY_SALT   = 37
DEFAULT_SECONDARY_SALT = 31


# ---------------------------------------------------------------------------
# Step 1: Parse the YAML file
# ---------------------------------------------------------------------------
def load_yaml(path):
    """
    Parse our simple YAML: lines of   &entity;: "value"   or   &entity;: 'value'
    No external libraries needed — the format is regular enough for a plain regex.
    Returns a dict of  { "&key;": "utf-8-value", ... }
    """
    entities = {}

    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue

            m = re.match(r'^(&[^:]+;):\s*"(.*)"$', line) or \
                re.match(r"^(&[^:]+;):\s*'(.*)'$",  line)

            if not m:
                print(f"  WARNING: could not parse line: {line!r}", file=sys.stderr)
                continue

            entities[m.group(1)] = m.group(2)

    return entities


# ---------------------------------------------------------------------------
# Step 2: Build the perfect hash tables
# ---------------------------------------------------------------------------
def poly_hash(key_bytes, salt, table_size):
    """
    Polynomial rolling hash:  h = (h * salt + byte) % table_size  for each byte.
    Must produce identical results to the C++ polyHash() in the generated file.
    """
    h = 0
    for byte in key_bytes:
        h = (h * salt + byte) % table_size
    return h


def build_perfect_hash(entities, primary_salt, secondary_salt):
    """
    Find displacement values so every key lands in a unique slot.

    Algorithm:
      1. Hash each key to a bucket using primary_salt.
      2. Sort buckets largest-first (bigger buckets are harder to place).
      3. For each bucket, try displacements 0, 1, 2, ... until all keys in
         the bucket land in currently-empty slots in the final table.

    Returns:
        table_size      - int = len(entities)  (minimal: zero wasted slots)
        displacements   - list[int], one displacement per bucket
        final_table_kv  - list of (key, value) or None, length = table_size
        empty_slots     - int (should be 0 for a minimal perfect hash)
    """
    keys       = list(entities.keys())
    n          = len(keys)
    table_size = n
    keys_bytes = [k.encode("utf-8") for k in keys]

    # --- Level 1: assign keys to buckets ---
    buckets = [[] for _ in range(table_size)]
    for i, kb in enumerate(keys_bytes):
        b = poly_hash(kb, primary_salt, table_size)
        buckets[b].append(i)   # store key index

    # Process larger buckets first — they are harder to place
    bucket_order = sorted(range(table_size), key=lambda b: -len(buckets[b]))

    # --- Level 2: find per-bucket displacements ---
    final_table   = [None] * table_size   # final_table[slot] = key_index
    displacements = [0]    * table_size   # displacement for each bucket

    for b_idx in bucket_order:
        bucket = buckets[b_idx]

        if not bucket:
            displacements[b_idx] = 0
            continue

        # Try displacements 0, 1, 2, ... until all keys in this bucket fit
        # into currently-empty slots without conflicting with each other.
        for displacement in range(table_size * 20):
            slots_for_bucket = []
            fits = True

            for key_idx in bucket:
                slot = (poly_hash(keys_bytes[key_idx], secondary_salt, table_size) + displacement) % table_size
                if final_table[slot] is not None or slot in slots_for_bucket:
                    fits = False
                    break
                slots_for_bucket.append(slot)

            if fits:
                # Commit: write all keys from this bucket into the final table
                for key_idx, slot in zip(bucket, slots_for_bucket):
                    final_table[slot] = key_idx
                displacements[b_idx] = displacement
                break
        else:
            raise RuntimeError(
                f"Could not place bucket {b_idx} (size {len(bucket)}) after "
                f"{table_size * 20} attempts. Try a different --primary-salt value."
            )

    # Convert slot indices -> (key, value) pairs
    final_table_kv = []
    for slot_key_idx in final_table:
        if slot_key_idx is None:
            final_table_kv.append(None)
        else:
            k = keys[slot_key_idx]
            final_table_kv.append((k, entities[k]))

    empty_slots = sum(1 for x in final_table_kv if x is None)
    return table_size, displacements, final_table_kv, empty_slots


# ---------------------------------------------------------------------------
# Step 3: Self-test — verify every entity round-trips correctly in Python
#         before we write any files
# ---------------------------------------------------------------------------
def self_test(entities, table_size, displacements, final_table_kv, primary_salt, secondary_salt):
    """Simulate the C++ lookup in Python to catch bugs before writing the file."""

    def lookup(key):
        kb     = key.encode("utf-8")
        bucket = poly_hash(kb, primary_salt,   table_size)
        slot   = (poly_hash(kb, secondary_salt, table_size) + displacements[bucket]) % table_size
        entry  = final_table_kv[slot]
        # Mirror the C++ check: compare length first, then content
        if entry is not None and len(kb) == len(entry[0].encode("utf-8")) and entry[0] == key:
            return entry[1]
        return None

    errors = 0
    for key, expected in entities.items():
        got = lookup(key)
        if got != expected:
            print(f"  FAIL: {key!r} expected {expected!r}, got {got!r}", file=sys.stderr)
            errors += 1

    if errors:
        raise RuntimeError(f"Self-test failed: {errors} lookup errors.")
    print(f"  Self-test passed: all {len(entities)} entities verified.")


# ---------------------------------------------------------------------------
# Step 4: Escape a Python string for use inside a C string literal
# ---------------------------------------------------------------------------
def c_escape(s):
    """
    Produce the body of a C string literal (without surrounding quotes).
    Non-ASCII bytes become \\xNN so the .cpp file stays pure ASCII.
    """
    result = []
    for byte in s.encode("utf-8"):
        if   byte == ord('"'):  result.append('\\"')
        elif byte == ord('\\'): result.append('\\\\')
        elif 0x20 <= byte <= 0x7E:
            result.append(chr(byte))
        else:
            result.append(f"\\x{byte:02X}")
    return "".join(result)


# ---------------------------------------------------------------------------
# Step 5: Emit the C++ source file
# ---------------------------------------------------------------------------
CPP_TEMPLATE = """\
// AUTO-GENERATED FILE - do not edit manually.
// Source of truth: htmlEntities.yaml
// Regenerate by running:  python3 scripts/generate_entities.py

#include "htmlEntities.h"
#include <cstring>


static const int PRIMARY_SALT   = {primary_salt};
static const int SECONDARY_SALT = {secondary_salt};
static const int TABLE_SIZE     = {table_size};

// Per-bucket displacement table
static const int DISPLACEMENTS[{table_size}] = {{
{displacement_rows}
}};

// Final hash table: each slot holds a key/value pair, or {{nullptr, nullptr, 0}}
struct EntitySlot {{
    const char* key;     // e.g. "&amp;"  - nullptr means this slot is empty
    const char* value;   // e.g. "&"      - UTF-8, non-ASCII as \\xNN escapes
    int         keyLen;  // byte length of key - avoids strlen() on every lookup
}};

static const EntitySlot HASH_TABLE[{table_size}] = {{
{table_rows}
}};

// Polynomial hash
static int polyHash(const char* key, int len, int salt) {{
    int h = 0;
    for (int i = 0; i < len; ++i) {{
        h = (h * salt + (unsigned char)key[i]) % TABLE_SIZE;
    }}
    return h;
}}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
const char* lookupHtmlEntity(const char* entity, int len) {{
    // Prevent hashing overhead for impossible lengths
    if (len < 4 || len > 33) {{
        return nullptr;
    }}

    int bucket          = polyHash(entity, len, PRIMARY_SALT);
    int slot            = (polyHash(entity, len, SECONDARY_SALT) + DISPLACEMENTS[bucket]) % TABLE_SIZE;
    const EntitySlot& s = HASH_TABLE[slot];
    if (s.key != nullptr && s.keyLen == len && memcmp(s.key, entity, len) == 0) {{
        return s.value;
    }}
    return nullptr;
}}
"""


def generate_cpp(entities, table_size, displacements, final_table_kv, empty_slots,
                 primary_salt, secondary_salt):
    # Displacement table — 10 per row for readability
    disp_rows = []
    for i in range(0, len(displacements), 10):
        chunk = displacements[i:i+10]
        disp_rows.append("    " + ", ".join(f"{d:3d}" for d in chunk))
    displacement_rows = ",\n".join(disp_rows)

    # Hash table rows — now include keyLen as the third field
    table_rows = []
    for entry in final_table_kv:
        if entry is None:
            table_rows.append('    {nullptr, nullptr, 0}')
        else:
            k, v = entry
            key_len = len(k.encode("utf-8"))
            table_rows.append(f'    {{"{c_escape(k)}", "{c_escape(v)}", {key_len}}}')
    table_rows_str = ",\n".join(table_rows)

    return CPP_TEMPLATE.format(
        entity_count      = len(entities),
        table_size        = table_size,
        empty_slots       = empty_slots,
        primary_salt      = primary_salt,
        secondary_salt    = secondary_salt,
        displacement_rows = displacement_rows,
        table_rows        = table_rows_str,
    )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main(primary_salt, secondary_salt):
    print(f"[generate_entities] Reading {YAML_PATH}")
    entities = load_yaml(YAML_PATH)
    print(f"  Loaded {len(entities)} entities")

    print(f"[generate_entities] Building perfect hash "
          f"(PRIMARY_SALT={primary_salt}, SECONDARY_SALT={secondary_salt})...")
    table_size, displacements, final_table_kv, empty_slots = \
        build_perfect_hash(entities, primary_salt, secondary_salt)
    print(f"  Table size: {table_size}, empty slots: {empty_slots}")

    print(f"[generate_entities] Running self-test...")
    self_test(entities, table_size, displacements, final_table_kv, primary_salt, secondary_salt)

    print(f"[generate_entities] Writing {OUTPUT_PATH}")
    cpp = generate_cpp(entities, table_size, displacements, final_table_kv, empty_slots,
                       primary_salt, secondary_salt)
    with open(OUTPUT_PATH, "w", encoding="utf-8") as f:
        f.write(cpp)

    print(f"[generate_entities] Done.")


# ---------------------------------------------------------------------------
# PlatformIO integration
# When PlatformIO runs this as an extra_script, the global `env` is injected.
# When run directly (python3 scripts/generate_entities.py), `env` doesn't
# exist and we fall through to the except branch and call main() directly.
# ---------------------------------------------------------------------------
try:
    env.AddPreAction(  # noqa: F821
        "$BUILD_DIR",
        lambda *args, **kwargs: main(DEFAULT_PRIMARY_SALT, DEFAULT_SECONDARY_SALT)
    )
except NameError:
    parser = argparse.ArgumentParser(
        description="Generate htmlEntities.cpp from htmlEntities.yaml"
    )
    parser.add_argument(
        "--primary-salt",
        type=int,
        default=DEFAULT_PRIMARY_SALT,
        help=f"Primary hash salt (default: {DEFAULT_PRIMARY_SALT})"
    )
    parser.add_argument(
        "--secondary-salt",
        type=int,
        default=DEFAULT_SECONDARY_SALT,
        help=f"Secondary hash salt (default: {DEFAULT_SECONDARY_SALT})"
    )
    args = parser.parse_args()
    main(primary_salt=args.primary_salt, secondary_salt=args.secondary_salt)
