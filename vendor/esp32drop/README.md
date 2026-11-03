# ESP32Drop provenance

Source: https://github.com/s-iwaki-d/ESP32Drop

Revision: `92bdc1221924ca522f3ccbebbd9353612ce5b2d7`, `ESP32Drop/` library directory.
Copyright MIRO; the upstream 0BSD license is retained in `LICENSE`.
Example sketches and their image assets are omitted. C/C++ formatting follows
CrossPoint's formatter.

CrossPoint changes add bounded request-metadata parsing, explicit acceptance,
streaming archive delivery through a main-task mailbox, transfer-size limits,
and checked receiver shutdown. AWDL queue and TLS task placement use PSRAM to
fit alongside the reader. The original sender interfaces remain in the library
but CrossPoint enables only reception.

`test/airdrop/run.py` exercises the protocol parsers, request framing, archive
streaming and callback mailbox under AddressSanitizer and UndefinedBehaviorSanitizer.
Real radio interoperability and resource headroom still require device tests.
