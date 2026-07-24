# XTC converter fixtures

`crossvi-converter-480x800.xtc` and `.xtch` are one-page, copyright-free
fixtures generated from a programmatic four-corner/four-gray test image by
`bigbag/epub-to-xtc-converter`, commit
`6792329e5ae77fac423db6b6cdc96af6afff82ae` (CLI `encoder.js`).

The fixtures freeze the converter's actual v1.0 byte contract:

- page-table `size` includes the 22-byte XTG/XTH header;
- page-header `dataSize` is payload-only;
- XTH stores bit 0 first and bit 1 second;
- chapter offsets are 64-bit and converter chapter page numbers are 1-based.

The image and metadata were created solely for CrossVi parser tests and do not
contain third-party book text or artwork.

SHA-256:

- XTC: `806ce1d93b72073e333622496e273a756f03157252355410d6c4a4c5c703d317`
- XTCH: `c233780e16d687f60f3a61c196f7b84e04d3f2c9f44e98abe77b84fa1afc3bfd`
