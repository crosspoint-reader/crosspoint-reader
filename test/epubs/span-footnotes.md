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
Requested text follows headings and widow/orphan lines carried to the next page;
partial caches do not publish a structural page while that text is still pending.
They also check the ordinary 1024-anchor cap, text-free structural targets beyond
the cap, TOC preservation, hidden/skipped IDs, BiDi and inserted-hyphen line
boundaries, leading soft hyphens/U+FEFF, targets in buffered grid cells, missing targets
and duplicate requested IDs. Text-free targets retain their structural page when
later text appears on another page, while empty children do not end target selection.
Empty requested non-TOC spans map to the following rendered text.
Inline targets also survive NFC composition and discarded U+FEFF within one word,
hyphenation, focus tokenization and a UTF-8 codepoint crossing the word-buffer boundary.
These cases reopen the completed anchor map and verify a page rather than a missing marker.
Anchor-map tests exercise completed negative lookups,
reopen, partial builds, legacy encoding, long IDs and truncated records.

For a broken link to an absent ID, follow it twice and then exit/reopen the book.
After the first completed build, that same target must keep the complete section
cache rather than restarting indexing. Inspect the anchor map in
`/.crosspoint/epub_<hash>/sections/<spine>.bin`: it contains one key starting with
byte `0x01` followed by the missing ID. The marker certifies only the requested
fragment for that cache; rebuilding for another fragment or render settings may
replace it.

For the v76 update, keep both a completed and a suspended v75 cache. After the
update, each should rebuild with the reading position preserved. Reopen cached
TXT and Markdown books and verify the saved position too.

Use EPUBs with the relevant markup in addition to the generated fixture. Check
links to drop caps, list text that starts on the next page, and horizontal rules
at a page break. Follow image links with a fresh section and after opening the
section normally to build its cache, both below and beyond the ordinary anchor
limit. Save a clipping containing only a list marker and another containing the
marker and text. Both should highlight the selected marker at the same layout.
After changing the layout, the mixed clipping's text should still highlight.

In an EPUB with nested backlinks, follow two links, then follow a backlink to
the intermediate page. Back should return to the original page. To check
interrupted links, sleep while a long destination chapter is still building. On
wake, the last rendered page and its earlier Back positions should be available.
