# DictHtmlRenderer Smoke Test

Host-side smoke test for `lib/DictHtmlRenderer/`. Verifies that the renderer
produces correct `StyledSpan` output for all 7 tag categories in the
`html-definitions` test dictionary, that the streaming render path matches the
batch path, exercises boundary conditions, and runs the IPA utility unit tests.

## Build and run

```bash
bash test/dict-html-renderer/run.sh
```

Requires `gcc` and `g++` (via `build-essential`). The script compiles the expat
C sources with `gcc` and the C++ sources (`DictHtmlRenderer.cpp`, `Utf8.cpp`,
`DictHtmlRendererTest.cpp`) with `g++`, links them, and runs the binary against
the test dictionary.

`DictHtmlRenderer.cpp` includes the device-only `HalStorage.h` (SD I/O for the
file-based render paths) and `Logging.h`. The script puts `test/dict-html-renderer/stubs/`
first on the include path, providing host stubs: a `HalFile` backed by a real
`FILE*` (so the file-based `renderFromFile*` paths work against actual `.dict`
files) and no-op `LOG_*` macros.

## Exit code

- **0** — all tests pass, no unexpected unknown tags
- **1** — one or more tests fail, or an unexpected unknown tag was encountered

## Output format

For each test the output shows:

- The `StyledSpan` list produced by the renderer (text, formatting flags)
- `PASS` or `FAIL`
- For failures: the expected spans alongside the actual spans for easy diff

At the end: `Results: N passed, N failed`.

## Test groups (15 tests)

### Group A: Dictionary entries (7 tests)

Each entry's full raw content is passed to `renderer.render()` — identical to how
on-device lookup code delivers a dictionary entry to the renderer. The expected
span tables cover the entire output: description paragraphs, `----------`
delimiter spans, test HTML spans, and the closing delimiter.

Dictionary headwords use opaque names (BlazeSilent, ClearSvg, etc.) to avoid
collisions with real dictionary words.

| Headword | Tag category |
|----------|-------------|
| BlazeSilent | `<abbr title="...">` expansion — renders as `text (title)` |
| ClearSvg | Tags stripped with all children: `hiero`, `svg`, `math`, `gallery`, `nowiki`, `poem`, `ref`, `REF`, `img` |
| DarkMath | Block structure: `p`, `div`, `br`, `blockquote`, `ol`+`li`, `ul`+`li`, `h1`–`h4` |
| EmptyGallery | Inline formatting: `b`, `strong`, `i`, `em`, `u`, `s`, `sub`, `sup`, `code`, `tt`, `small`, `big`, `var`; nested combinations |
| FrostNowiki | Strip-tag-keep-text: `span`, `a` (anchor), plus intentional unknown tags (expected — does not fail) |
| GlowPoem | Wikitext annotation self-closing tags: `t:XX`, `tr:XX`, `lang:XX`, `gloss:XX`, `pos:XX`, `sc:XX`, `alt:XX`, `id:XX`; plus body-text suppression (ninth case) |
| HazeEntity | HTML named entities: `&lsqb;`/`&rsqb;` (brackets), `&nbsp;` (non-breaking space), `&ndash;` (en dash), `&lrm;` (zero-width, dropped), unknown entity (dropped) |

### Group D: Streaming parity (1 test)

For every Group A entry, `renderFromFileStreaming()` (reading the `.dict` file via
the `HalStorage` stub and delivering spans through a sink) must produce exactly
the same spans as the batch `render()`. This guards the streaming span-sink path
(which never materializes the whole-definition `textBuf`/spans vector) against
divergence from the reference batch path.

### Group B: Boundary tests (5 tests)

Programmatically generated inputs that exercise the (dynamically sized) renderer.

| Test | What it verifies |
|------|-----------------|
| B1: parseError | Malformed XML (`<p>unclosed`) returns the partial output (1 span, `"unclosed"`) accumulated before the parse error |
| B2: large input | 100 paragraphs × 90 chars (9000 bytes) — all 100 paragraphs produce spans (dynamic buffers, no truncation) |
| B3: long paragraph | Single 600-char paragraph — emitted as exactly 1 span (no fixed pending-text limit) |
| B4: deep nesting | 35 nested `<i>` tags — deepest text still renders italic (dynamic tag stack, no fixed depth) |
| B5: control chars | `\n` between plain text and `<p>` triggers a line break, not a glyph (F-060/F-061 regression) |

### Group C: IPA utility tests (2 tests)

Unit tests for `IpaUtils.h` functions, compiled and run alongside the renderer tests.

| Test | What it verifies |
|------|-----------------|
| isIpaCodepoint | Range boundaries (IPA Extensions / Modifier Letters U+0250–U+02FF, Phonetic Extensions U+1D00–U+1DBF) and the individual IPA letters added outside those ranges (U+00E6, U+00F0, U+0153, U+03B2, U+03B8); non-IPA codepoints rejected |
| splitIpaRuns | Empty string, pure ASCII, single IPA codepoint, mixed ASCII+IPA+ASCII, consecutive IPA codepoints merging into one run, and a combining mark attaching to the current run |

## Unknown tag tracking

The smoke test binary is compiled with `-DDICT_HTML_RENDERER_TRACK_UNKNOWN`. Any
tag not in the renderer's registry causes an `ERROR` line and a non-zero exit.
The FrostNowiki entry is exempt — it deliberately contains unknown tags to verify
the strip-keep fallback; those are printed as `INFO` and do not cause failure.

## What this test does NOT verify

- E-ink display rendering (requires hardware)
- Font metrics, line wrapping, or pagination (covered host-side by the separate
  `test/dict-layout/` litmus)
- Any tag not present in the `html-definitions` test dictionary
