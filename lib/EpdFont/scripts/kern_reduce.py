"""Fit a font's kerning into the classes the reader can store, losing as little as possible.

Kerning is stored by class: left code points whose kerning rows are identical share a left class,
right code points whose columns are identical share a right class, and a class x class matrix holds
the values. A class number is one byte in the file and in the firmware, so a side may have at most
255 classes. A font that needed more used to lose all its kerning (fontconvert_sdcard.py) or got
tables the reader misreads (fontconvert.py).

reduce_kern_classes() leaves a table that fits untouched. Otherwise it merges classes, cheapest
first, until each side fits:
  - merging a class into another gives its code points the other's kerning;
  - a class can also merge into "no kerning", which suits rows of tiny values;
  - the cost is how much kerning changes, each pair weighted by how much its characters matter
    (ASCII most, then the everyday letters of Latin, Greek and Cyrillic, then everything else), and
    multiplied by the weight of the class that changes - so rare characters absorb the loss.
The left side is reduced first, then the right. Merging columns keeps identical rows identical, so
the left side never grows again.

The browser converter does the same in tools/ftwasm/js/kern-reduce.mjs, and the two must give the
same result: test/web/kern_reduce_test.mjs compares them. Hence only integer arithmetic, and a fixed
order: candidates are scanned by class index (classes numbered by their lowest code point), a class's
"no kerning" merge before its merges with later classes, and the first cheapest candidate wins.

Pure Python, no dependencies: imported by fontconvert_sdcard.py and fontconvert.py.
"""

KERN_CLASS_LIMIT = 255


def cp_weight(cp):
    """How much a code point's kerning matters when classes have to be merged."""
    if 0x20 <= cp < 0x7F:
        return 16
    if 0xC0 <= cp <= 0x17F or 0x391 <= cp <= 0x3C9 or 0x410 <= cp <= 0x44F:
        return 4
    return 1


def _profiles(kern_map, side):
    """{cp: {other_cp: value}} for one side: rows (left) or columns (right)."""
    rows = {}
    for (left, right), value in kern_map.items():
        key, other = (left, right) if side == "left" else (right, left)
        rows.setdefault(key, {})[other] = value
    return rows


def _classes(rows):
    """Identical profiles grouped, numbered by their lowest code point: (profiles, members)."""
    profiles, members, by_signature = [], [], {}
    for cp in sorted(rows):
        signature = tuple(sorted(rows[cp].items()))
        index = by_signature.get(signature)
        if index is None:
            index = by_signature[signature] = len(profiles)
            profiles.append(rows[cp])
            members.append([])
        members[index].append(cp)
    return profiles, members


def count_kern_classes(kern_map):
    """(left classes, right classes) the table needs. The map holds no zero values."""
    return (len(_classes(_profiles(kern_map, "left"))[0]),
            len(_classes(_profiles(kern_map, "right"))[0]))


def _distance(a, b):
    total = 0
    for key, value in a.items():
        total += abs(value - b.get(key, 0)) * cp_weight(key)
    for key, value in b.items():
        if key not in a:
            total += abs(value) * cp_weight(key)
    return total


def _reduce_side(kern_map, side, cap):
    profiles, members = _classes(_profiles(kern_map, side))
    n = len(profiles)
    weight = [sum(cp_weight(cp) for cp in m) for m in members]
    norm = [sum(abs(v) * cp_weight(k) for k, v in p.items()) for p in profiles]
    distance = [[0] * n for _ in range(n)]
    for i in range(n):
        for j in range(i + 1, n):
            distance[i][j] = _distance(profiles[i], profiles[j])
    alive = [True] * n
    count = n
    while count > cap:
        best_cost, best_from, best_to = None, -1, -1
        for i in range(n):
            if not alive[i]:
                continue
            cost = weight[i] * norm[i]  # into "no kerning"
            if best_cost is None or cost < best_cost:
                best_cost, best_from, best_to = cost, i, -1
            for j in range(i + 1, n):
                if not alive[j]:
                    continue
                if weight[i] <= weight[j]:
                    cost, frm, to = weight[i] * distance[i][j], i, j
                else:
                    cost, frm, to = weight[j] * distance[i][j], j, i
                if cost < best_cost:
                    best_cost, best_from, best_to = cost, frm, to
        alive[best_from] = False
        count -= 1
        if best_to >= 0:
            members[best_to].extend(members[best_from])
            weight[best_to] += weight[best_from]
    out = {}
    for i in range(n):
        if not alive[i]:
            continue
        for cp in members[i]:
            for other, value in profiles[i].items():
                out[(cp, other) if side == "left" else (other, cp)] = value
    return out


def reduce_kern_classes(kern_map, cap=KERN_CLASS_LIMIT):
    """(kern_map, info): the table, merged to at most `cap` classes per side if it needed more.

    info is None when the table already fitted (and is returned as it was), else
    {"left": (before, after), "right": (before, after), "pairs_changed": n}.
    """
    left_before, right_before = count_kern_classes(kern_map)
    if left_before <= cap and right_before <= cap:
        return kern_map, None
    reduced = kern_map
    if left_before > cap:
        reduced = _reduce_side(reduced, "left", cap)
    if count_kern_classes(reduced)[1] > cap:
        reduced = _reduce_side(reduced, "right", cap)
    left_after, right_after = count_kern_classes(reduced)
    keys = set(kern_map) | set(reduced)
    changed = sum(1 for k in keys if kern_map.get(k, 0) != reduced.get(k, 0))
    return reduced, {"left": (left_before, left_after), "right": (right_before, right_after),
                     "pairs_changed": changed}
