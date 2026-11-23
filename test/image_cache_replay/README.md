# Host image framebuffer and cache regressions

```sh
cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/test --target ImageCacheReplayTest
ctest --test-dir build/test -R '^(ImageCacheReplayTest|GifFramebufferTest|ImageScalingTest|ImagePackedWriterTest)\.' --output-on-failure
```

The tests use the production converters and cache replay on the host, with
scalar ImageOps. CMake reuses the host AnimatedGIF target and fetches the
firmware-pinned PNGdec and JPEGDEC sources without patching them.

Small encoded fixtures and a synthetic cache exercise sampling, clipping,
transparency, ordered dithering, RAM/stream replay, and allocation and write
failures. Framebuffers and cache files are compared in memory against expected
bytes. No captures are written and no reference from an older revision is needed.

Scaling tests link a second copy of the production PNG/JPEG converters with
acceleration disabled and test-only class names. The same nonuniform input is
decoded by both versions; complete framebuffer buffers (including guards) and
cache files must match. RGB PNGs also have independent sampling/luminance
expectations. Cases cover cropping, fine/coarse downscaling, upscaling, mixed-axis
scales, all four orientations, BW and both gray planes, absolute/overlay planes,
strip and screen clipping, and optional portrait-scratch OOM. JPEG comparisons
preserve the disabled converter's sampling and interpolation behavior; they do
not independently validate the JPEG decoder. Cropped JPEG callbacks can paint
outside the nominal image rectangle in the disabled path, so those cases compare
framebuffers and caches separately rather than assuming cache replay reproduces
the entire direct framebuffer. Small JPEG outputs may also use the existing
no-cache fallback when a decode block does not fit the cache band.

Packed-writer tests feed production `Bitmap::readNextRow()` output into the
production row sink in both BMP row orders. They check partial-byte batches,
cropped columns, negative placement, strip/top/bottom clipping, transparency,
planes, immediate buffer reuse, repeated flushes, short-read destruction, and
allocation-failure fallback. They test the sink lifecycle, not the full
`GfxRenderer::drawBitmap()` setup. Host tests do not execute S3 assembly.
