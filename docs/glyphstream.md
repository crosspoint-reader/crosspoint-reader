# GlyphStream v1: Built-in Font Compression

GlyphStream is the compressed storage format for the firmware's built-in font
bitmaps (Noto Serif/Sans reader fonts and the Ubuntu / Noto Sans 8pt UI
fonts). Every glyph is stored as an **independent compressed stream** that can
be decoded on its own in tens of microseconds. It replaced two older schemes:
per-script-group DEFLATE for the 2-bit reader fonts, and *no compression at
all* for the 1-bit UI fonts.

Code map: format constants in [lib/EpdFont/EpdFontData.h](../lib/EpdFont/EpdFontData.h)
(`EPD_BITMAP_FORMAT_GLYPH_STREAM_V1`), decoder in
[lib/EpdFont/GlyphStreamCodec.{h,cpp}](../lib/EpdFont/GlyphStreamCodec.h),
runtime integration in [lib/EpdFont/FontDecompressor.cpp](../lib/EpdFont/FontDecompressor.cpp),
encoder/trainer in [lib/EpdFont/scripts/](../lib/EpdFont/scripts/), model in
[lib/EpdFont/builtinFonts/glyphStreamModel.h](../lib/EpdFont/builtinFonts/glyphStreamModel.h),
host tests in [test/glyph_stream/](../test/glyph_stream/).

## Why

1. **Flash.** Fonts were the largest single consumer of app-partition flash:
   1,336 KiB of bitmap arrays (2,415 KiB including metadata). GlyphStream
   removes 556 KiB of that (default-environment flash usage: ~87% with the
   legacy formats, 78.5% with GlyphStream).
2. **RAM.** The old group-DEFLATE path inflated an entire script group (up to
   a 64 KB transient allocation) to extract one glyph. GlyphStream decodes
   one glyph at a time using two fixed 3 KB scratch planes.
3. **Random access.** The 1-bit UI fonts could never be compressed before:
   UI rendering fetches individual glyphs on demand, and a group format
   can't serve that. Per-glyph streams can — the UI fonts compressed to
   0.38× their raw size (Ubuntu 12pt regular: 0.33×).
4. **Pay only for what you use.** The stream is layered so black-and-white
   consumers (all UI text) decode roughly half the bytes and stop.

## Results (measured, shipped fonts)

| class | before | after | ratio |
|---|---:|---:|---:|
| Noto Serif, 16 fonts (2-bit, was DEFLATE) | 638,746 B | 396,974 B | 0.62× |
| Noto Sans, 16 fonts (2-bit, was DEFLATE) | 591,042 B | 362,880 B | 0.61× |
| UI fonts, 5 fonts (1-bit, was uncompressed) | 137,963 B | 52,231 B | 0.38× |
| **all bitmap arrays** | **1,367,751 B** | **812,085 B** | **0.59×** |

Shared model cost (all fonts, forever): 7,150 B — three decision trees
(893 nodes × 6 B) plus 896 leaf probabilities (× 2 B).

Sample fonts:

| font | before | after | ratio |
|---|---:|---:|---:|
| notoserif_18_bolditalic | 56,478 | 31,923 | 0.57× |
| notoserif_16_regular | 38,333 | 24,615 | 0.64× |
| notosans_12_regular | 24,467 | 17,305 | 0.71× |
| ubuntu_12_regular | 32,540 | 10,734 | 0.33× |
| notosans_8_regular | 18,647 | 10,384 | 0.56× |

Smaller point sizes compress less (fewer pixels to amortize per-glyph
overhead); bold/large sizes compress best.

## How it works

Five ideas stack. Each is illustrated with real data from the shipped
`notoserif_16_regular` (pixel legend, two columns per pixel: `· ` white, `░░` light gray,
`▒▒` dark gray, `██` black).

### 1. One stream per glyph

The existing `EpdGlyph` metadata locates each stream (`dataOffset` /
`dataLength` — no struct changes). A stream is:

```
byte 0     header: bit7 HAS_REF, bit6 RAW, bits0-3 vertical shift + 7
bytes 1-2  base glyph index, u16 LE (only when HAS_REF)
rest       one range-coded payload: layer 1 (ink), then layer 2 (gray)
```

`RAW` marks the fallback where compression would not have helped; the payload
is then the legacy packed bitmap verbatim. It is rare (6 of 1,041 glyphs in
notoserif_16_regular) but guarantees the format never loses to raw storage.

### 2. Base-glyph references — 'ä' borrows 'a'

Fonts are full of near-duplicate shapes: accented letters contain their base
letter, and Cyrillic А/Е/о are pixel-identical to Latin A/E/o. A glyph may
therefore name an earlier same-width glyph as its **base layer**, with a
vertical shift to align baselines. Real shipped streams:

```
'a' (base)                          'ä' (target)                        pixels the coder
30-byte stream                      17-byte stream                      actually "pays" for
· · · · · · · · · · · · · · · · ·   · · · · ▒▒▒▒· · · · ▒▒██░░· · · ·   · · · · ▒▒▒▒· · · · ▒▒██░░· · · · 
· · · · · · · · · · · · · · · · ·   · · · ██████░░· · ░░██████· · · ·   · · · ██████░░· · ░░██████· · · · 
· · · · · · · · · · · · · · · · ·   · · · ██████▒▒· · ░░██████· · · ·   · · · ██████▒▒· · ░░██████· · · · 
· · · · · · · · · · · · · · · · ·   · · · ░░████· · · · ████░░· · · ·   · · · ░░████· · · · ████░░· · · · 
· · · · · · · · · · · · · · · · ·   · · · · · · · · · · · · · · · · ·   · · · · · · · · · · · · · · · · · 
· · · · · · · · · · · · · · · · ·   · · · · · · · · · · · · · · · · ·   · · · · · · · · · · · · · · · · · 
· · · · · · · · · · · · · · · · ·   · · · · · · · · · · · · · · · · ·   · · · · · · · · · · · · · · · · · 
· · · ░░▒▒████████████▒▒· · · · ·   · · · ░░▒▒████████████▒▒· · · · ·   · · · · · · · · · · · · · · · · · 
· · ░░██████▒▒░░▒▒████████· · · ·   · · ░░██████▒▒░░▒▒████████· · · ·   · · · · · · · · · · · · · · · · · 
· · ██████░░· · · · ▒▒████▒▒· · ·   · · ██████░░· · · · ▒▒████▒▒· · ·   · · · · · · · · · · · · · · · · · 
· · ██████· · · · · · ██████· · ·   · · ██████· · · · · · ██████· · ·   · · · · · · · · · · · · · · · · · 
· · ░░████· · · · · · ██████· · ·   · · ░░████· · · · · · ██████· · ·   · · · · · · · · · · · · · · · · · 
· · · · · · · · · · · ██████· · ·   · · · · · · · · · · · ██████· · ·   · · · · · · · · · · · · · · · · · 
· · · · · · · · · · · ██████· · ·   · · · · · · · · · · · ██████· · ·   · · · · · · · · · · · · · · · · · 
· · · · · · · ░░░░░░░░██████· · ·   · · · · · · · ░░░░░░░░██████· · ·   · · · · · · · · · · · · · · · · · 
· · · ░░████████████████████· · ·   · · · ░░████████████████████· · ·   · · · · · · · · · · · · · · · · · 
· · ████████░░· · · · ██████· · ·   · · ████████░░· · · · ██████· · ·   · · · · · · · · · · · · · · · · · 
· ▒▒████▒▒· · · · · · ██████· · ·   · ▒▒████▒▒· · · · · · ██████· · ·   · · · · · · · · · · · · · · · · · 
· ██████· · · · · · · ██████· · ·   · ██████· · · · · · · ██████· · ·   · · · · · · · · · · · · · · · · · 
░░██████· · · · · · · ██████· · ·   ░░██████· · · · · · · ██████· · ·   · · · · · · · · · · · · · · · · · 
░░██████· · · · · · ░░██████· · ·   ░░██████· · · · · · ░░██████· · ·   · · · · · · · · · · · · · · · · · 
· ██████· · · · · · ████████· · ·   · ██████· · · · · · ████████· · ·   · · · · · · · · · · · · · · · · · 
· ██████▒▒· · · · ▒▒▒▒▒▒████░░· ·   · ██████▒▒· · · · ▒▒▒▒▒▒████░░· ·   · · · · · · · · · · · · · · · · · 
· ░░████████████████· ░░██████░░·   · ░░████████████████· ░░██████░░·   · · · · · · · · · · · · · · · · · 
· · ░░██████████▒▒· · ░░████████░░  · · ░░██████████▒▒· · ░░████████░░  · · · · · · · · · · · · · · · · · 
· · · · · ░░░░· · · · · · · · · ·   · · · · · ░░░░· · · · · · · · · ·   · · · · · · · · · · · · · · · · · 
```

'ä' (17×26 = 442 pixels, 111 B packed raw) references 'a' at shift −6. Only
**27 of 442 pixels** differ from the aligned base — the umlaut — so the
stream costs 17 bytes, *less than the base itself* (30 B). Homoglyphs are
even starker: Cyrillic 'А' is a 14-byte stream referencing Latin 'A' at shift
0, while 'A' itself (the reference-free root) costs 40 bytes.

Two non-obvious lessons are baked into the encoder:

- **There is no explicit diff.** The base is *context*, not an XOR layer:
  each target pixel is coded conditioned on both its own causal neighborhood
  and the aligned base pixel. This is mathematically equivalent to coding a
  residual, but the model also keeps seeing the true image structure — a
  residual-of-residuals model measurably performs worse.
- **References are found by measured coded cost, not Unicode knowledge.**
  The encoder shortlists earlier same-width glyphs by ink overlap, then
  keeps a reference only if the actual coded size shrinks by more than the
  header cost. It rediscovers 'ä'→'a' and 'А'→'A' on its own, plus
  non-obvious shape pairs no table would list. In notoserif_16_regular,
  54% of glyphs carry a reference. Chains are capped at depth 2.

### 3. Layered planes — ink first, gray on top

A 2-bit glyph is coded in two passes over the same pixels:

- **Layer 1 — ink plane**: one bit per pixel, "ink or white". This is
  exactly what 1-bit rendering needs, and all it decodes (~55% of stream
  bytes). The 1-bit UI fonts are *only* this layer.
- **Layer 2 — gray refinement**: revisits ink pixels and codes their level
  (light/dark/black) as two binary decisions.

The same 'a' from above, decomposed:

```
full 2-bit glyph                    layer 1: ink plane                  layer 2: gray refinement
(reader, Text AA)                   (all a BW consumer decodes)         (boundary pixels only)
· · · · · · · · · · · · · · · · ·   · · · · · · · · · · · · · · · · ·   · · · · · · · · · · · · · · · · · 
· · · ░░▒▒████████████▒▒· · · · ·   · · · ██████████████████· · · · ·   · · · ░░▒▒· · · · · · ▒▒· · · · · 
· · ░░██████▒▒░░▒▒████████· · · ·   · · ██████████████████████· · · ·   · · ░░· · · ▒▒░░▒▒· · · · · · · · 
· · ██████░░· · · · ▒▒████▒▒· · ·   · · ████████· · · · ████████· · ·   · · · · · ░░· · · · ▒▒· · ▒▒· · · 
· · ██████· · · · · · ██████· · ·   · · ██████· · · · · · ██████· · ·   · · · · · · · · · · · · · · · · · 
· · ░░████· · · · · · ██████· · ·   · · ██████· · · · · · ██████· · ·   · · ░░· · · · · · · · · · · · · · 
· · · · · · · · · · · ██████· · ·   · · · · · · · · · · · ██████· · ·   · · · · · · · · · · · · · · · · · 
· · · · · · · · · · · ██████· · ·   · · · · · · · · · · · ██████· · ·   · · · · · · · · · · · · · · · · · 
· · · · · · · ░░░░░░░░██████· · ·   · · · · · · · ██████████████· · ·   · · · · · · · ░░░░░░░░· · · · · · 
· · · ░░████████████████████· · ·   · · · ██████████████████████· · ·   · · · ░░· · · · · · · · · · · · · 
· · ████████░░· · · · ██████· · ·   · · ██████████· · · · ██████· · ·   · · · · · · ░░· · · · · · · · · · 
· ▒▒████▒▒· · · · · · ██████· · ·   · ████████· · · · · · ██████· · ·   · ▒▒· · ▒▒· · · · · · · · · · · · 
· ██████· · · · · · · ██████· · ·   · ██████· · · · · · · ██████· · ·   · · · · · · · · · · · · · · · · · 
░░██████· · · · · · · ██████· · ·   ████████· · · · · · · ██████· · ·   ░░· · · · · · · · · · · · · · · · 
░░██████· · · · · · ░░██████· · ·   ████████· · · · · · ████████· · ·   ░░· · · · · · · · · ░░· · · · · · 
· ██████· · · · · · ████████· · ·   · ██████· · · · · · ████████· · ·   · · · · · · · · · · · · · · · · · 
· ██████▒▒· · · · ▒▒▒▒▒▒████░░· ·   · ████████· · · · ████████████· ·   · · · · ▒▒· · · · ▒▒▒▒▒▒· · ░░· · 
· ░░████████████████· ░░██████░░·   · ██████████████████· ██████████·   · ░░· · · · · · · · · ░░· · · ░░· 
· · ░░██████████▒▒· · ░░████████░░  · · ██████████████· · ████████████  · · ░░· · · · · ▒▒· · ░░· · · · ░░
· · · · · ░░░░· · · · · · · · · ·   · · · · · ████· · · · · · · · · ·   · · · · · ░░░░· · · · · · · · · · 
```

Of the 'a''s 340 pixels, 146 are ink and only 36 of those are gray — and
every gray pixel sits on a stroke edge. A black-and-white consumer stops
after the middle column and never pays for the right one.

The reason layering wins over coding 2-bit values directly: gray pixels live
almost exclusively on stroke boundaries (look at the 'a' art above — every
`░`/`▒` hugs a `█` edge). Because layer 2 runs after layer 1 completes, its
context can ask "is there ink *below* this pixel?" — information a
single-pass causal coder can never see, and the strongest single predictor
of gray-vs-black.

### 4. A decision-tree probability model, not context tables

**What a context model is.** Compression is prediction. A *context model*
assigns every upcoming symbol a probability based on its *context* — the
data already decoded. If the model says "99% ink" and the pixel is indeed
ink, the range coder (§5) charges −log2(0.99) ≈ 0.01 bits; a 50/50 guess
costs a full bit. Nothing is transformed or approximated — the model only
sets the price of each outcome, and because the decoder rebuilds the same
context from its own output, encoder and decoder stay in lockstep without
any side information. All the compression skill is in asking the context
the right questions. For glyphs, the context that matters is the 2-D pixel
neighborhood:

```
NW  N  ·
 W  ?  ·        raster order: everything above and left of ? is
 ·  ·  ·        already decoded and can be used to predict it
```

**The simplest version first.** A classic bilevel-image model ("ctx3") looks
at just those three already-decoded neighbors — West, North, North-West —
giving 8 possible contexts, each with one probability measured offline from
real fonts. These are the actual statistics of notoserif_16_regular's ink
plane:

```
neighbors (W N NW)      p(ink)   typical cost   share of pixels
none ink                0.035     0.05 bits         52.0%
N only  (stroke above)  0.753     0.41 bits          6.3%
W+N ink, NW white       0.996     0.01 bits          1.5%
all three ink           0.930     0.10 bits         23.3%
```

Read it as glyph anatomy: three-quarters of all pixels are "blank background
continues" or "stroke interior continues", and those cost 1/10th of a bit or
less. The expensive pixels are the ambiguous ones — stroke edges, where a
vertical stem may or may not continue. Painting each pixel of the 'a' with
its ctx3 coding cost makes this visible:

```
legend (darker = more expensive):
· <0.1 bits     ░░ <0.5     ▒▒ <1.5     ██ ≥1.5

· · · · · · · · · · · · · · · · · 
· · · ██░░░░░░░░░░░░░░░░██· · · · 
· · ██· ░░░░░░░░░░░░░░░░████· · · 
· · ░░░░░░░░██▒▒▒▒▒▒▒▒░░░░████· · 
· · ░░░░░░██· · · · ██▒▒░░░░░░· · 
· · ░░░░░░░░· · · · · ░░░░░░░░· · 
· · ██▒▒▒▒· · · · · · ░░░░░░░░· · 
· · · · · · · · · · · ░░░░░░░░· · 
· · · · · · · ██░░░░░░· ░░░░░░· · 
· · · ██░░░░░░· ░░░░░░░░░░░░░░· · 
· · ██· ░░░░░░██▒▒▒▒▒▒▒▒░░░░░░· · 
· ██· ░░░░██▒▒· · · · ░░░░░░░░· · 
· ░░░░░░██· · · · · · ░░░░░░░░· · 
██· ░░░░░░· · · · · · ░░░░░░░░· · 
░░░░░░░░░░· · · · · ██· ░░░░░░· · 
██▒▒░░░░░░· · · · · ░░░░░░░░░░· · 
· ░░░░░░████· · · ██· ░░░░░░████· 
· ░░░░░░░░██░░░░░░· ██▒▒░░░░░░████
· ██▒▒░░░░░░░░░░░░██· ░░░░░░░░░░██
· · ██▒▒▒▒▒▒░░██▒▒· · ██▒▒▒▒▒▒▒▒▒▒
```

Background and stroke interiors are nearly free; the cost concentrates in a
thin rim along the stroke boundary. Summed up, the 'a''s 340-pixel ink plane
costs 169 bits ≈ 21 bytes under this 8-entry model — versus 43 bytes for
raw 1-bit storage. Even the simplest context model beats byte-oriented
compressors on glyph data, because it sees the 2-D structure a byte stream
hides.

**Why not just add neighbors?** Every extra neighbor *doubles* the flat
table while halving the training data behind each entry: 12 neighbors is
already 4,096 probabilities, most backed by too few samples to trust. Yet
the measurements say wider windows keep helping well past 12 features.

The resolution is to *learn* the context at build time instead of fixing it
by hand. The training script grows a decision tree over a wide candidate
window, greedily choosing at every node the single feature whose answer
most reduces the corpus's coded size — so the offline build discovers which
questions carry information, and only those ever become nodes. A candidate
feature that never earns a split simply doesn't exist in the shipped model.
The result inverts the flat-table trade-offs:

- **Flash**: probabilities exist only where the data proves a distinction
  matters — a few hundred leaves instead of 2^18 table entries, and every
  leaf is backed by enough training samples to be trustworthy.
- **Compute**: classifying a pixel is a ~6–10 step branch walk over a
  ~5 KB structure, instead of assembling an 18-bit index into a table too
  large to stay cache-friendly.

At decode time GlyphStream gathers the feature window and walks the tree to
one of a few hundred probability classes:

- Layer 1 features (18): twelve causal ink neighbors, four
  position-in-glyph flags (near left/right/top/bottom edge), base-plane ink
  at the pixel and below it.
- Layer 2 features (17): the previous row's and current row's decoded gray
  levels (W/N/NE/NW), south-side ink from the completed layer 1, the base
  plane's gray level, and the position flags.

The real trained ink tree's first two questions (from
`glyphStreamModel.h`, feature ids: 1 = north ink, 16 = base ink):

```c
static constexpr GlyphStreamTreeNode kInkTree[] = {
    { 1, 1, 233 },     // root: is the pixel above ink?
    { 16, 2, 156 },    // no ink above: does the BASE glyph have ink here?
    ...
```

The tree spends leaves only where the data distinguishes outcomes: 512
leaves for ink, 256 + 128 for gray, ~7 KB total. Measured against flat
tables, 64 leaves match a 4096-entry table at 1/21 the size.

One model serves every font. This was measured, not hoped: trees trained
only on one typeface code the others within ~1% of specially trained trees —
stroke statistics generalize across serif/sans/UI faces and sizes. New fonts
added later need no retraining.

Putting the section's pieces on one scale — notoserif_16_regular's complete
bitmap data under each scheme:

| scheme | bytes | vs raw |
|---|---:|---:|
| raw packed 2-bit | 118,609 | 1.00× |
| ctx3 + simplest gray model, per glyph † | 45,136 | 0.38× |
| DEFLATE script groups (previous format) | 38,333 | 0.32× |
| GlyphStream trees, references disabled † | 34,102 | 0.29× |
| **GlyphStream as shipped (trees + references)** | **24,615** | **0.21×** |

† produced by the real encoder (same flush, RAW fallback, and header
accounting as shipped) with the stated model substituted and references
disabled; ctx3 probabilities are cross-typeface trained like the shipped
model. Every row in this table is actual coder output.

Note the second and third rows: a simple per-glyph context model *loses* to
DEFLATE, despite predicting pixels well — DEFLATE's back-references exploit
the cross-glyph redundancy that a per-glyph model cannot see. That is
exactly the gap the base-glyph references (§2) close, and then some.

### 5. Static binary range coding

Leaf probabilities feed an LZMA-style binary range coder (16-bit
probabilities, no adaptation — decode needs zero model state). The coder's
whole trick is interval arithmetic: it maintains a number interval of width
1.0, and every coded pixel shrinks it by exactly the probability the model
assigned to what actually happened. The output is just enough bits to name
one number inside the final interval — so a whole output bit only
materializes each time the width halves:

```
width 1.000  ████████████████████████████████████████████████████████ 
      0.965  ██████████████████████████████████████████████████████   background pixel: ×0.965   (0.05 bits)
      0.932  ████████████████████████████████████████████████████     another one
        ⋮
      0.494  ████████████████████████████                             after 20 background pixels → one whole bit
      0.181  ██████████                                               ONE ambiguous pixel (p=0.366, is ink) → 1.45 bits
```

Confident pixels barely shrink the interval: twenty background pixels
together cost one bit, while a single genuinely ambiguous pixel costs more
than that on its own. This is how a pixel can cost 0.05 bits — fractional
bits are real, not an accounting fiction. It is also how 'ä''s 442 pixels
fit in a 17-byte stream.

Summing this accounting over the whole 'a' ink plane (the §4 heatmap) shows
where the bytes actually go:

```
'a' ink plane under ctx3: 340 pixels → 169 bits ≈ 21 bytes

 150 px  background & interior     █                               7 bits    4%
 130 px  cheap edges               █████                          29 bits   17%
  24 px  uncertain edges           ████                           21 bits   13%
  36 px  surprises (stroke starts) ██████████████████            112 bits   66%
```

Nearly half the pixels — background and stroke interiors — cost 4% of the
bits; the 11% of pixels the context could *not* foresee (stroke onsets,
sharp turns) cost two-thirds. The coder spends its budget precisely on
genuine information: where strokes begin.

Two size conventions matter across ~40,000 glyphs: the always-zero
leading byte is omitted, and the encoder finishes by picking the value with
the most zero tail bytes from the final interval — any value inside it
decodes identically — so the trailing flush bytes truncate away (the
decoder zero-fills past the payload end). That choice alone is worth ~3
bytes per glyph, ~110 KB across the fleet.

A consequence of that truncation: corruption of a coded payload is **not
detectable at decode time** — the decoder always terminates (bit count is
fixed by width × height) and is memory-safe, but wrong bytes produce wrong
pixels, silently. Integrity is a build-time guarantee instead: the encoder
decodes back every stream it writes and the fleet validator
(`validate_glyphstream_fonts.py`) compares every glyph of every font
pixel-exact against the rasterizer output. This matches the trust model of
the flash-resident data it replaced, which had no runtime checks either.

## Runtime behavior

- `FontDecompressor` dispatches on `fontData->bitmapFormat`; the legacy
  group-DEFLATE and raw paths remain for `EPD_BITMAP_FORMAT_LEGACY`.
- Decoding uses two statically allocated 3 KB scratch planes
  (`GlyphStreamCodec::SCRATCH_PLANE_SIZE`); a reference chain decodes
  grand-base → base → target across them. Max glyph is 63×46 pixels
  (encoder-enforced).
- Reader page rendering prewarms needed glyphs into the existing page-slot
  cache; UI fonts decode through the same path on demand.
- Malformed structure (bad base index, width mismatch, chain > 2, oversized
  dims, short RAW payload) fails with `LOG_ERR` + skip, never a crash.

## Toolchain

- **Regenerate fonts**: `lib/EpdFont/scripts/convert-builtin-fonts.sh`
  drives `fontconvert.py`, which encodes GlyphStream by default for built-in
  fonts. `fontconvert.py --dump-bitmaps out.npz` exports raw rasterized
  bitmaps (training input / validation ground truth).
- **Retrain the model**: `train_glyphstream_model.py` (deterministic — no
  randomness; ties broken by fixed rules) regenerates `glyphStreamModel.h`.
  Bump `GLYPH_STREAM_MODEL_VERSION` and regenerate *all* fonts together:
  streams are only decodable with the model they were encoded against.
- **Validate**: `validate_glyphstream_fonts.py` decodes every glyph of every
  generated header and compares against ground truth. Run it after any
  encoder, model, or font change.
- **Host tests**: `test/glyph_stream/` — Python↔C++ roundtrip fixtures
  (regenerated by `test/glyph_stream/generate_fixtures.py`) plus adversarial
  decoder inputs. The Python encoder and C++ decoder must remain bit-exact
  mirrors; treat any coder-convention change as a format version bump.

## Invariants for future changes

1. **Model and fonts version together.** Never commit a model change without
   regenerating every font header (and vice versa).
2. **The coder pair is bit-exact.** Any change to range-coder conventions,
   feature definitions, or tree semantics is a new `bitmapFormat`, not an
   edit.
3. **`EpdFontData` field order matters to generated headers**, which use
   positional initializers ending at `bitmapFormat`. New fields must be
   appended after it (or headers regenerated), or aggregate initialization
   breaks.
4. **Streams never beat raw guaranteed**: keep the RAW fallback path when
   touching the encoder.

## Future work

The bitmaps were only half of the font flash story. Glyph *metadata* —
`EpdGlyph` arrays, kern tables, intervals — still occupies 1,102 KiB,
uncompressed, and host-side analysis shows most of it is padding and zeros.
Three tracks, in rough order of value per risk:

1. **Pack the glyph records (no codec involved).** `EpdGlyph` is a padded
   16-byte struct, but the measured field ranges across all 37 fonts are
   tiny: width ≤ 63, height ≤ 46, advance ≤ 1,038, left ∈ [−28, 9],
   top ∈ [−3, 41] — and `dataLength` is derivable for GlyphStream (decode
   is driven by pixel count). An 8-byte record (w/h/advance/left/top +
   chunked stream offsets) is bit-exact and needs no decoder.
2. **Sparse kern matrices.** The class-pair matrices are 88.8% zeros
   (38,083 nonzero of 338,672 cells). CSR-style storage (row pointers +
   (column, value) pairs) keeps lookups RAM-only with a short row scan
   after the existing binary searches — also bit-exact.
3. **Metrics as stream layer 0.** The logical endpoint of the layered
   design: glyph metrics carry only ~17 bits of real information each under
   simple conditional coding (h given w, advance given w, ...), and ~30% of
   glyphs share an identical metrics tuple with an earlier glyph — the same
   redundancy structure the base references already exploit. Coding them as
   a "layer 0" at the head of each stream would let text measurement decode
   ~25 bits and stop, dissolve the kern class-entry tables into the stream,
   and shrink the external per-glyph footprint to just a chunked offset.
   Cost: a cold `getGlyph()` becomes a short decode instead of a flash
   pointer read, so metrics must ride the existing glyph caches; it also
   changes `EpdGlyph`-facing interfaces, which is why it is staged last.

Host-measured potential (savings are estimates from the same measurement
methodology that predicted GlyphStream's shipped size within 0.3%):

| track | basis | est. flash saved |
|---|---|---:|
| GlyphStream v1 (shipped) | actual | 556 KB |
| 8-byte glyph records + chunked offsets | field-range audit | ~317 KB |
| sparse kern matrices (CSR) | zero-density audit | ~255 KB |
| metrics as layer 0 | conditional-entropy measurement | ~200 KB |
| **total potential (incl. shipped)** | | **~1.33 MB of the 2.4 MB font flash** |

