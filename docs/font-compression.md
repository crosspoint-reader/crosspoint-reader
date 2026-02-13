

## Flash & RAM

| | baseline | font-compression | Difference |
|--|--------|-----------------|------------|
| Flash (ELF) | 6,302,476 B (96.2%) | 4,365,022 B (66.6%) | -1,937,454 B (-30.7%) |
| firmware.bin | 6,468,192 B | 4,531,008 B | -1,937,184 B (-29.9%) |
| RAM | 101,700 B (31.0%) | 103,076 B (31.5%) | +1,376 B (+0.5%) |

## Script-Based Grouping (Cold Cache)

Comparison of uncompressed baseline vs script-based group compression (4-slot LRU cache, cleared each page). Glyphs are grouped by Unicode block (ASCII, Latin-1, Latin Extended-A, Combining Marks, Cyrillic, General Punctuation, etc.) instead of sequential groups of 8.

### Render Time

| | Baseline | Compressed (cold cache) | Difference |
|---|---|---|---|
| **Median** | 414.9 ms | 431.6 ms | +16.7 ms (+4.0%) |
| **Pages** | 37 | 37 | |

### Memory Usage

| | Baseline | Compressed (cold cache) | Difference |
|---|---|---|---|
| **Heap free (median)** | 187.0 KB | 176.3 KB | -10.7 KB |
| **Heap free (min)** | 186.0 KB | 166.5 KB | -19.5 KB |
| **Largest block (median)** | 148.0 KB | 128.0 KB | -20.0 KB |
| **Largest block (min)** | 148.0 KB | 120.0 KB | -28.0 KB |

### Cache Effectiveness

| | Misses/page | Hit rate |
|---|---|---|
| **Compressed (cold cache)** | 2.1 | 99.85% |

---

