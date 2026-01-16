# Hypher Binary Tries

CrossPoint embeds the exact binary automata produced by
[Typst's `hypher`](https://github.com/typst/hypher). 

## File layout

Each `.bin` blob is a single self-contained automaton:

```
uint32_t root_addr_be;  // big-endian offset of the root node
uint8_t  levels[];      // shared "levels" tape (dist/score pairs)
uint8_t  nodes[];       // node records packed back-to-back
```

The size of the `levels` tape is implicit. Individual nodes reference slices
inside that tape via 12-bit offsets, so no additional pointers are required.

### Node encoding

Every node starts with a single control byte:

- Bit 7 – set when the node stores scores (`levels`).
- Bits 5-6 – stride of the target deltas (1, 2, or 3 bytes, big-endian).
- Bits 0-4 – transition count (values ≥ 31 spill into an extra byte).

If the `levels` flag is set, two more bytes follow. Together they encode a
12-bit offset into the global `levels` tape and a 4-bit length. Each byte in the
levels tape packs a distance/score pair as `dist * 10 + score`, where `dist`
counts how many UTF-8 bytes we advanced since the previous digit.

After the optional levels header come the transition labels (one byte per edge)
followed by the signed target deltas. Targets are stored as relative offsets
from the current node address. Deltas up to ±128 fit in a single byte, larger
distances grow to 2 or 3 bytes. The runtime walks the transitions with a simple
linear scan and materializes the absolute address by adding the decoded delta
to the current node’s base.

## Embedding blobs into the firmware

The helper script `scripts/generate_hyphenation_trie.py` acts as a thin
wrapper: it reads the hypher-generated `.bin` files, formats them as `constexpr`
byte arrays, and emits headers under
`lib/Epub/Epub/hyphenation/generated/`. Each header defines the raw data plus a
`SerializedHyphenationPatterns` descriptor so the reader can keep the automaton
in flash.

To refresh the firmware assets after updating the `.bin` files, run:

```
./scripts/generate_hyphenation_trie.py \
    --input lib/Epub/Epub/hyphenation/tries/en.bin \
    --output lib/Epub/Epub/hyphenation/generated/hyph-en.trie.h

./scripts/generate_hyphenation_trie.py \
    --input lib/Epub/Epub/hyphenation/tries/fr.bin \
    --output lib/Epub/Epub/hyphenation/generated/hyph-fr.trie.h

./scripts/generate_hyphenation_trie.py \
    --input lib/Epub/Epub/hyphenation/tries/de.bin \
    --output lib/Epub/Epub/hyphenation/generated/hyph-de.trie.h

./scripts/generate_hyphenation_trie.py \
    --input lib/Epub/Epub/hyphenation/tries/ru.bin \
    --output lib/Epub/Epub/hyphenation/generated/hyph-ru.trie.h
```
