# DictHtmlRenderer Smoke Test

Host-side smoke test for `lib/DictHtmlRenderer/`. Verifies that the renderer
produces correct `StyledSpan` output for all 6 tag categories in the
`expat-full` test dictionary, and that no unknown tags are encountered.

## Build and run

```bash
bash test/run_dict_html_renderer.sh
```

Requires `gcc` and `g++` (via `build-essential`). The script compiles the
expat C sources with `gcc` and the C++ sources with `g++`, links them, and
runs the binary against the test dictionary.

## Exit code

- **0** — all 6 entries pass, no unexpected unknown tags
- **1** — one or more entries fail, or an unexpected unknown tag was encountered

## Output format

For each entry the test prints:

- The raw HTML extracted from the dictionary
- The `StyledSpan` list produced by the renderer (text, formatting flags)
- `PASS` or `FAIL`
- For failures: the expected spans alongside the actual spans for easy diff

At the end: `Results: N passed, N failed`.

## How entries are processed

Each entry's full raw content is passed to `renderer.render()` — identical to
how on-device lookup code delivers a dictionary entry to the renderer. The
expected span tables cover the entire output: description paragraphs,
`----------` delimiter spans, test HTML spans, and the closing delimiter.

## What is tested

| Entry | Tag category |
|-------|-------------|
| AbbrExpand | `<abbr title="...">` expansion — renders as `text (title)` |
| BlockStrip | Tags stripped with all children: `hiero`, `svg`, `math`, `gallery`, `nowiki`, `poem`, `ref`, `REF` |
| BlockStruct | Block structure: `p`, `div`, `br`, `blockquote`, `ol`+`li`, `ul`+`li`, `h1`–`h4` |
| FormatTags | Inline formatting: `b`, `strong`, `i`, `em`, `u`, `s`, `sub`, `sup`, `code`, `tt`, `small`, `big`, `var` |
| StripKeep | Strip-tag-keep-text: `span`, plus intentional unknown tags (expected — does not fail) |
| WikiAnnot | Wikitext annotation self-closing tags: `t:XX`, `tr:XX`, `lang:XX`, `gloss:XX`, `pos:XX`, `sc:XX`, `alt:XX`, `id:XX` |

## Unknown tag tracking

The smoke test binary is compiled with `-DDICT_HTML_RENDERER_TRACK_UNKNOWN`.
Any tag not in the renderer's registry causes an `ERROR` line and a non-zero
exit. The `StripKeep` entry is exempt — it deliberately contains unknown tags
to verify the strip-keep fallback; those are printed as `INFO` and do not
cause failure.

## What this test does NOT verify

- E-ink display rendering (requires hardware)
- Font metrics, line wrapping, or pagination
- Any tag not present in the `expat-full` test dictionary
