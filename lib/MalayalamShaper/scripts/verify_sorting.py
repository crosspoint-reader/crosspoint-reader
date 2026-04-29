#!/usr/bin/env python3
"""Verify that shaping rule tables are sorted correctly for binary search."""
import re
import sys

with open("lib/MalayalamShaper/MalayalamShapingData.h") as f:
    content = f.read()

# Verify Rule2 sorting
rule2_entries = re.findall(r"\{(0x[0-9A-F]+), (0x[0-9A-F]+), 0x[0-9A-F]+\}", content[:content.find("rules2Count")])
prev = (0, 0)
sorted_ok = True
for a, b in rule2_entries:
    cur = (int(a, 16), int(b, 16))
    if cur < prev:
        print(f"UNSORTED Rule2: {prev} > {cur}")
        sorted_ok = False
        break
    prev = cur
status = "OK" if sorted_ok else "FAIL"
print(f"Rule2 ({len(rule2_entries)} entries): {status}")

# Verify Rule3 sorting
rule3_section = content[content.find("rules3[]"):content.find("rules3Count")]
rule3_entries = re.findall(r"\{(0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), 0x[0-9A-F]+\}", rule3_section)
prev = (0, 0, 0)
sorted_ok = True
for a, b, c in rule3_entries:
    cur = (int(a, 16), int(b, 16), int(c, 16))
    if cur < prev:
        print(f"UNSORTED Rule3: {prev} > {cur}")
        sorted_ok = False
        break
    prev = cur
status = "OK" if sorted_ok else "FAIL"
print(f"Rule3 ({len(rule3_entries)} entries): {status}")

if not sorted_ok:
    sys.exit(1)
