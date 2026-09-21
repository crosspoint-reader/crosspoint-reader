# KOReader percentage-parser lifetime

This target compiles the production chapter resolver with the vendored Expat
implementation. Test-only create/free hooks install Expat's supported memory
allocator callbacks and count live parser handles and allocations. The parser,
XML callbacks and destructor path are real; the EPUB stub supplies chunked XHTML
and controlled read failures without an SD card.

The lifetime checks require one live parser at a time and no allocations from
the counting pass when the resolving parser is created. Existing percentage
anchors, invalid/empty/malformed input and allocation/read failures are also
checked. This verifies reduced overlap, not a fixed number of saved bytes on
device or an ESP32 heap measurement.
