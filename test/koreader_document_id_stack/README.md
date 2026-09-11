# Document ID scratch-buffer tests

This suite compiles the real `KOReaderDocumentId.cpp` and `Memory.h`. A linker
wrapper intercepts only the nothrow array allocation overload to inject a failed
1024-byte scratch allocation. Successful allocations delegate to the original
runtime overload, and ordinary allocation and `delete[]` remain unchanged. A
memory-backed HAL fixture supplies file content, while a host `MD5Builder`
adapter uses OpenSSL to compute an independent digest.

Install the OpenSSL development package before configuring the host tests
(`libssl-dev` on Debian/Ubuntu). Linux and CI require it; other hosts skip this
suite with a status message if OpenSSL cannot be found. The firmware gains no
dependency.

```sh
cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/test --target KOReaderDocumentIdStackTest
ctest --test-dir build/test -R '^DocumentIdStack[./]' --output-on-failure
```

Allocation fault injection requires a native 64-bit Linux toolchain with the
Itanium C++ ABI and a linker that supports `--wrap`. CMake compiles, links and
runs a probe that verifies interception of `_ZnamRKSt9nothrow_t` and delegation
to the original allocator. Compiler identity alone does not enable the wrapper.
If the probe fails or the host is unsupported, the target still runs its digest,
sampling and API tests; only the allocation-failure test is skipped and allocation
counts are not asserted. The configure log reports which path is active.

OpenSSL adapter failures are logged and recorded as GoogleTest failures, even
when the caller expects an empty result. The adapter preserves the firmware's
void API, clears its digest on failure and ignores subsequent calls. Fault
injection covers context allocation, initialization, update and finalization;
tests verify diagnostics, context cleanup and persistent failure state.

To exercise the same fallback on a supported host, configure a separate build
with `-DKOREADER_DOCUMENT_ID_STACK_FORCE_NO_WRAP=ON` and run the target above.

Fixture bytes follow `(position * 37 + position / 251) % 256`. Expected digests
were calculated with Python `hashlib.md5` over the concatenated official samples:
offset 0, followed by `1024 << (2 * i)` for `i = 0..10`, up to 1024 bytes per
offset. The last sample is limited by the advertised file size.

This refactor changes scratch-buffer ownership only. Existing short-read,
negative-read and failed-seek behavior is outside its scope and requires a
separate correction; these tests do not endorse incomplete hashes or unsafe
length conversions. The host adapter refuses oversized hash input without
reading outside the fixture buffer.

Host tests verify the checked allocation and unchanged hashes. Firmware stack
measurements and physical-device testing remain separate checks; moving scratch
storage to the heap does not reduce total RAM by 1024 bytes or prove an existing
stack overflow.
