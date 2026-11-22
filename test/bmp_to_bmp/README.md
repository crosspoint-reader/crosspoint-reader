# BMP correctness tests

Build with the existing host-test CMake configuration:

```sh
cmake --build build/test --target BmpToBmpTest BitmapRowC3EnabledTest BitmapRowS3ModelTest
ctest --test-dir build/test --output-on-failure -R 'BmpToBmpTest|BitmapPackedRowTest|BitmapGrayRowTest|BitmapBgrRowTest'
```

`BitmapRowC3EnabledTest` runs the full row matrix with optimized scalar ImageOps.
`BmpToBmpTest` runs the converter tests and representative rows with the default
non-S3 paths. `BitmapRowS3ModelTest` runs only the representative rows and checks
which kernels dispatch. Host models do not execute hardware SIMD instructions.

`BitmapPackedRowTest.cpp` compares `Bitmap::readNextRow()` against independent
per-pixel palette mapping and packing. It covers all input byte values and all
1/2-bit palette-to-level mappings, identity/reversed/noncanonical/partial palettes,
odd widths, zeroed tails, BMP padding, both row orders, rewinds, and output
guards. It also checks 4-bit quantization and dithering across chunk boundaries,
8-bit fallback, and recovery after a short read without advancing dither state.
The S3 model checks that native shortcuts bypass unpacking and that the real
2- and 4-bit fallback paths call it.

`BitmapGrayRowTest` generates BMP bytes and calculates expected luminance from
independent bit extraction and `(77R + 150G + 29B) / 256`. Cases cover 1/2/4/8-bit
indices, colored and short palettes, every packed byte, widths 1..2048, padding,
unaligned and exact-sized buffers, both row orders, repeated row reads, file
positions, read/seek failures and unchanged sequential dithering state.

`BitmapBgrRowTest` checks both row APIs against generated RGB values and their
independently calculated luminance. It covers BGR24 and unchanged BGRA32 controls,
primaries/mixed colors, row order/padding, widths 1..2048, chunk/vector tails,
unaligned and exact-sized buffers, both dither threshold sets, unchanged dither
state across gray reads, file positions, read/seek failures and ignored alpha.
S3 model counts require the BGR coefficients in both 24-bit paths and no SIMD
conversion for 32-bit rows. The ImageOps host suite separately checks every
24-bit color in both channel orders, all byte alignments and exact span bounds.

The S3 model also checks that the grayscale row API calls all three indexed
unpackers. These tests do not measure performance or use Git revisions as
reference behavior.
