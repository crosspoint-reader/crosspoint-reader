# Span footnote navigation regression

Generate the fixture with Python (standard library only):

```sh
python3 scripts/generate_span_footnote_test_epub.py
```

The EPUB contains 151 Russian footnotes in a separate XHTML spine, each wrapped
in `<span id="idN">` containing block elements. The TOC points only to the two
documents, without fragments. Another 10,000 empty converter span IDs precede
the notes. Links 1, 28 and 151 appear first for convenient testing.

1. Open the fixture on unpatched firmware and visit the notes chapter so a cache
   without the span anchors is created. Keep that cache when updating firmware.
2. On patched firmware follow links 28 and 151, using the link picker or touch.
   Each must open the page displaying the corresponding note number.
3. Use Back to return to the source position. Repeat the same link, choose a
   different note, exit the book, reopen, and repeat.
4. Clear this book's reading cache and repeat with a fresh build. Change the font
   size and test portrait, inverted portrait and both landscape orientations.
5. With debug logs enabled, check `ERS: Resolved anchor 'id28' to page ...` and
   `Heap before section build`. On C3 also monitor minimum free heap and largest
   free block over repeated jumps; check that the converter IDs do not cause
   growing retained heap use. Hardware validation is required for these checks.

The host parser tests compare the selected anchor with the page actually holding
its text, including a page boundary and an inline span inside a long paragraph.
They also check the ordinary 1024-anchor cap, text-free structural targets beyond
the cap, TOC preservation, hidden/skipped IDs, BiDi and inserted-hyphen line
boundaries, leading soft hyphens, targets in buffered grid cells, missing targets
and duplicate requested IDs. Anchor-map tests exercise completed negative lookups,
reopen, partial builds, legacy encoding, long IDs and truncated records.

For a broken link to an absent ID, follow it twice and then exit/reopen the book.
After the first completed build, that same target must keep the complete section
cache rather than restarting indexing. Inspect the anchor map in
`/.crosspoint/epub_<hash>/sections/<spine>.bin`: it contains one key starting with
byte `0x01` followed by the missing ID. The marker certifies only the requested
fragment for that cache; rebuilding for another fragment or render settings may
replace it.
