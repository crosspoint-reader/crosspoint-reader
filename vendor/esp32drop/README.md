# ESP32Drop provenance

Source: https://github.com/s-iwaki-d/ESP32Drop

Revision: `92bdc1221924ca522f3ccbebbd9353612ce5b2d7`, `ESP32Drop/` library directory.
Copyright MIRO; the upstream 0BSD license is retained in `LICENSE`.
Example sketches and their image assets are omitted. C/C++ formatting follows
CrossPoint's formatter.

CrossPoint changes add bounded request-metadata parsing, explicit acceptance,
streaming archive delivery through a main-task mailbox, transfer-size limits,
and checked receiver shutdown. AWDL queue and TLS task placement use PSRAM to
fit alongside the reader.

This fork contains only the streaming receiver. Sender/BLE wake-up APIs,
buffered archive delivery, unused certificate alternatives, and unused status
accessors and unconsumed packet-dump buffers are omitted. TLS uses CrossPoint's existing wolfSSL dependency with
TLS 1.2 server support enabled only for AirDrop builds; the upstream 0BSD license does
not replace wolfSSL's own license.

The 16-slot receive ring reserves 25,984 bytes of internal RAM for an active
AWDL session. Allocation failure stops startup. Teardown releases it only after
the workers and Wi-Fi callbacks stop; failed teardown retains the storage and
requires caller recovery. This reduces idle memory use, not active peak memory.
The radio remains exclusive to AWDL while reception is enabled.

The optimized transport requires fresh iPhone/device interoperability testing;
host TLS tests do not establish radio timing or device heap headroom.

`test/airdrop/run.py` exercises the protocol parsers, request framing, archive
streaming and callback mailbox under AddressSanitizer and UndefinedBehaviorSanitizer.
Real radio interoperability and resource headroom still require device tests.

The optional native TLS check compiles the actual adapter against the downloaded,
patched wolfSSL sources and connects using Python/OpenSSL:

```sh
python3 test/airdrop/tls_test.py --wolfssl-source .pio/libdeps/x4c-gh_release/Arduino-wolfSSL/src
```

It checks TLS 1.2 negotiation, multi-record payload checksums, reconnection, and
session resumption, TLS 1.3 client negotiation, and rejection by the disabled
TLS 1.3 server entry point. It needs a host C/C++ toolchain and a loopback socket.

The compact receiver removes the unused census, plist writers and write-only
AWDL diagnostics. The receive histogram still controls channel timing. Gauge
histogram rotation is in place and needs no permanent scratch buffer.

`scripts/generate_airdrop_identity.py` runs before compilation and creates the
ignored `ad_identity.h` from the fixed X4 Pro/X4C receiver identities. Changing
the advertised name requires rebuilding firmware. Incoming filenames and
metadata still use the existing parser.

`scripts/patch_wolfssl.py` excludes TLS 1.3 server branches in the pinned
`tls13.c`, leaving TLS 1.2 reception and TLS 1.3 client support enabled. It checks
the upstream source hash and fails on an unknown version; dependency upgrades
require reviewing the patch and rerunning the native TLS tests. Shared wolfSSL
headers and structure layouts are unchanged.
