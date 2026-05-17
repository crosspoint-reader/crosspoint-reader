# PR 2: "fix: Image Rendering"

## Goal
- Render image-only EPUB sections, including SVG wrapper pages that reference raster images.
- Avoid decoding/rendering images during non-visible text measurement and grayscale passes.

## What Was Broken
- Some EPUB cover pages are not plain `<img src="...">` documents. They are small XHTML/SVG wrapper pages whose visible raster cover is referenced from an SVG `<image>` element.
- The slim chapter parser only treated HTML `<img src>` as image content, so an SVG wrapper such as `<image href="cover.jpg">` or `<image xlink:href="cover.jpg">` could build a valid section without producing any `PageImage` content. The reader then drew a blank or text-only page even though the EPUB contained a cover image.
- Once image pages did render, the normal page pipeline could also touch images during font prewarm/measurement and text anti-aliasing grayscale passes. Those passes are not user-visible image draws, so decoding images there wasted time and could duplicate cache work.

## What Changed
- The parser now recognizes both HTML and SVG image elements:
  - `<img src="...">`
  - `<image href="...">`
  - `<image xlink:href="...">`
- SVG-wrapped raster images reuse the same EPUB-relative path resolution as existing image handling, so paths relative to the current XHTML/SVG wrapper resolve consistently.
- Image content is serialized as page image content and rendered only by the visible page pass.
- Text scan/prewarm and grayscale text anti-aliasing passes use text-only rendering, so image decode/cache work is not repeated for invisible passes.
- Existing image-rendering settings are preserved:
  - display images
  - placeholders
  - suppress images

## Scope
- EPUB chapter image parsing for `img`, SVG `image href`, and SVG `image xlink:href`.
- Page render helpers that let visible rendering include images while measurement/text-only passes skip them.
- Image diagnostics for parsed, resolved, skipped, rejected, cache, and decode paths.

## Verification
- `python test/image_rendering_static_contracts.py`
- `./bin/clang-format-fix`
- `pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high`
- `pio run -e default`

---

### AI Usage

While CrossPoint doesn't have restrictions on AI tools in contributing, please be transparent about their usage as it 
helps set the right context for reviewers.

Did you use AI tools to help write this code? _**YES**_
