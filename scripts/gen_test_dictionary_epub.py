#!/usr/bin/env python3
"""
Generate test/epubs/test_dictionary.epub

Dictionary usage (3 switches after initial setup):
  Ch 1   Scanner validation  : no-ifo, only-dict, multi-ifo, fail-decompress
  Ch 2   Pre-processing      : gen-idx, gen-syn, extract-dict,
                               extract-syn-gen-syn, all-prep  ->  cleanup: select full
  Ch 3-10  full block        : full  (26 words + 22 synonyms, English)
  Ch 11  No-syn              : basic                (switch #1)
  Ch 12  Bilingual edge case : en-es                (switch #2)
  Ch 13  Phrase (Phase 6)   : phrase / phrase-syn  (switch #3, final)

Non-ASCII / entity audit:
  &gt;   required XML escape for '>' (kept)
  &#160; non-breaking space in '[ ]' pending prefix to prevent whitespace
         collapse (kept -- glyph is identical to space, font support universal)
  All em dashes (U+2014) replaced with --
  All arrow entities (&#8594;) replaced with ->
"""

import os, zipfile

OUT = os.path.join(os.path.dirname(__file__), "..", "test", "epubs", "test_dictionary.epub")

CHAPTERS = []

def ch(title, body):
    CHAPTERS.append((title, body))

# ---------------------------------------------------------------------------
# Ch 1 -- Scanner Validation
# ---------------------------------------------------------------------------
ch("1. Dictionary Picker -- Scanner Validation", """
<h1>1. Dictionary Picker -- Scanner Validation</h1>

<p>Place all test dictionaries in /dictionary/ on the SD card.
No word lookups are performed in this chapter.
Open Settings -> Dictionary for every test below.</p>

<h2>Test A: Folder without .ifo is hidden</h2>
<p><strong>Dictionary:</strong> no-ifo</p>
<p>The no-ifo folder must not appear in the picker list.</p>
<ul>
<li>Open the dictionary picker.</li>
<li>Confirm no-ifo is absent from the list.</li>
</ul>

<h2>Test B: Folder with only .dict is hidden</h2>
<p><strong>Dictionary:</strong> only-dict</p>
<p>The only-dict folder must not appear in the picker list.</p>
<ul>
<li>Open the dictionary picker.</li>
<li>Confirm only-dict is absent from the list.</li>
</ul>

<h2>Test C: Folder with multiple .ifo files is hidden</h2>
<p><strong>Dictionary:</strong> multi-ifo</p>
<p>The multi-ifo folder must not appear in the picker list.</p>
<ul>
<li>Open the dictionary picker.</li>
<li>Confirm multi-ifo is absent from the list.</li>
</ul>

<h2>Test D: Valid dictionaries appear in picker</h2>
<p>All of the following must be visible in the picker. Do not select any yet.</p>
<ul>
<li>basic</li>
<li>en-es</li>
<li>fail-decompress</li>
<li>full</li>
<li>phrase</li>
<li>phrase-syn</li>
</ul>
<p>Pre-processing dictionaries (gen-idx, gen-syn, extract-dict,
extract-syn-gen-syn, all-prep) are also valid and appear here.
They are exercised in Chapter 2.</p>

<h2>Test E: Failed decompression -- error shown, prior selection unchanged</h2>
<p><strong>Dictionary:</strong> fail-decompress</p>
<p>The preparation must fail with an error. The picker must remain open
and the previously selected dictionary must be unchanged. No crash.</p>
<ul>
<li>Note the currently selected dictionary before starting.</li>
<li>Select fail-decompress in the picker.</li>
<li>The preparation screen appears listing one step: Extract dictionary</li>
<li>Press Continue.</li>
<li>Screen shows: Preparation failed</li>
<li>Picker remains open. Previously selected dictionary is unchanged.</li>
</ul>
""")

# ---------------------------------------------------------------------------
# Ch 2 -- Pre-Processing
# ---------------------------------------------------------------------------
ch("2. Pre-Processing", """
<h1>2. Pre-Processing</h1>

<p>Each test selects a dictionary that requires one or more preparation steps.
Open Settings -> Dictionary for each test.
No word lookups are needed in this chapter.</p>

<p><strong>Expected confirmation screen:</strong></p>
<ul>
<li>Each required step listed by name.</li>
<li>This may take over 10 minutes.</li>
<li>Plug device into charger before continuing.</li>
</ul>

<p><strong>Expected processing screen:</strong></p>
<ul>
<li>Each step visible with a status prefix throughout:
    [ &gt; ] in-progress (bold),
    [OK] complete (regular),
    [&#160;&#160;&#160;] pending (regular).</li>
<li>On success: Preparation complete appears below the step list.</li>
</ul>

<h2>Test A: Generate dictionary offset file only</h2>
<p><strong>Dictionary:</strong> gen-idx</p>
<p>Confirmation screen must list exactly one step.
That step must complete with [OK] and Preparation complete must appear.</p>
<ul>
<li>Select gen-idx in the picker.</li>
<li>Confirmation screen lists: Generate dictionary offset file</li>
<li>Press Continue.</li>
<li>Step completes: [OK] Generate dictionary offset file</li>
<li>Preparation complete.</li>
</ul>

<h2>Test B: Generate synonym offset file only</h2>
<p><strong>Dictionary:</strong> gen-syn</p>
<p>Confirmation screen must list exactly one step.
That step must complete with [OK] and Preparation complete must appear.</p>
<ul>
<li>Select gen-syn in the picker.</li>
<li>Confirmation screen lists: Generate synonym offset file</li>
<li>Press Continue.</li>
<li>Step completes: [OK] Generate synonym offset file</li>
<li>Preparation complete.</li>
</ul>

<h2>Test C: Extract dictionary only</h2>
<p><strong>Dictionary:</strong> extract-dict</p>
<p>Confirmation screen must list exactly one step.
That step must complete with [OK] and Preparation complete must appear.</p>
<ul>
<li>Select extract-dict in the picker.</li>
<li>Confirmation screen lists: Extract dictionary</li>
<li>Press Continue.</li>
<li>Step completes: [OK] Extract dictionary</li>
<li>Preparation complete.</li>
</ul>

<h2>Test D: Extract synonyms then generate synonym offset file</h2>
<p><strong>Dictionary:</strong> extract-syn-gen-syn</p>
<p>Confirmation screen must list exactly two steps in order.
Each step must show in-progress bold while running, then [OK] regular
on completion. Preparation complete must appear after both finish.</p>
<ul>
<li>Select extract-syn-gen-syn in the picker.</li>
<li>Confirmation screen lists in order:</li>
<li>Extract synonyms</li>
<li>Generate synonym offset file</li>
<li>Press Continue.</li>
<li>Both steps run sequentially, each showing [ &gt; ] then [OK].</li>
<li>Preparation complete.</li>
</ul>

<h2>Test E: All four preparation steps</h2>
<p><strong>Dictionary:</strong> all-prep</p>
<p>Confirmation screen must list exactly four steps in order.
Each step must complete with [OK] before the next begins.
Preparation complete must appear after all four finish.</p>
<ul>
<li>Select all-prep in the picker.</li>
<li>Confirmation screen lists in order:</li>
<li>Extract dictionary</li>
<li>Extract synonyms</li>
<li>Generate dictionary offset file</li>
<li>Generate synonym offset file</li>
<li>Press Continue.</li>
<li>All four steps run sequentially.</li>
<li>Preparation complete.</li>
</ul>

<p><strong>Cleanup:</strong> select full before proceeding to Chapter 3.
This dictionary is used through Chapter 10.</p>
""")

# ---------------------------------------------------------------------------
# Ch 3 -- Direct Lookup
# ---------------------------------------------------------------------------
ch("3. Direct Lookup", """
<h1>3. Direct Lookup</h1>

<p><strong>Setup:</strong> full selected.</p>

<h2>Test A: Definition returned immediately</h2>
<p>Each word below is a headword in full. Selecting it must open the
definition screen directly with no synonym prompt or suggestions screen.</p>
<ul>
<li>apple</li>
<li>cloud</li>
<li>dawn</li>
<li>flame</li>
<li>grove</li>
<li>harvest</li>
<li>ivory</li>
<li>jungle</li>
<li>kettle</li>
<li>lantern</li>
<li>meadow</li>
<li>ocean</li>
<li>pilgrim</li>
<li>riddle</li>
<li>shadow</li>
<li>valley</li>
<li>willow</li>
<li>zenith</li>
</ul>

<h2>Test B: Done button on direct hit</h2>
<p>After any successful lookup from Test A, the Confirm button on the
definition screen must be labelled Done. This confirms showDoneButton
is set correctly for direct hits.</p>
<ul>
<li>Look up any word from Test A.</li>
<li>Verify Confirm button label is Done, not Synonyms.</li>
</ul>
""")

# ---------------------------------------------------------------------------
# Ch 4 -- Case Normalization
# ---------------------------------------------------------------------------
ch("4. Case Normalization", """
<h1>4. Case Normalization</h1>

<p><strong>Setup:</strong> full selected.</p>
<p>Words below appear capitalised at the start of a sentence as they would
in normal reading. The lookup must normalise to lowercase before searching
the index. Each capitalised word must resolve to a definition with no
not-found message and no synonym prompt.</p>

<h2>Test A: Capitalised sentence-initial words resolve correctly</h2>
<p>Read the passage below, then select each word marked in the list.</p>
<p>Ocean tides rise and fall each day.
Shadow falls across the valley floor.
Flame leaps high in the still night air.
Dawn breaks slowly over the distant ridge.
Meadow grasses bend gently in the wind.
Harvest time brings the whole village together.
Willow branches trail softly in the stream.</p>
<ul>
<li>Ocean</li>
<li>Shadow</li>
<li>Flame</li>
<li>Dawn</li>
<li>Meadow</li>
<li>Harvest</li>
<li>Willow</li>
</ul>
""")

# ---------------------------------------------------------------------------
# Ch 5 -- Index Boundaries
# ---------------------------------------------------------------------------
ch("5. Index Boundaries", """
<h1>5. Index Boundaries</h1>

<p><strong>Setup:</strong> full selected.</p>
<p>These words sit at the very start and end of the full index,
exercising the binary search at its boundaries. Each must return
a definition without errors.</p>

<h2>Test A: Words at the start of the index</h2>
<ul>
<li>apple</li>
<li>bridge</li>
<li>cloud</li>
<li>dawn</li>
<li>echo</li>
</ul>

<h2>Test B: Words at the end of the index</h2>
<ul>
<li>xenon</li>
<li>yearn</li>
<li>zenith</li>
</ul>
""")

# ---------------------------------------------------------------------------
# Ch 6 -- Stem Variants
# ---------------------------------------------------------------------------
ch("6. Stem Variants", """
<h1>6. Stem Variants</h1>

<p><strong>Setup:</strong> full selected.</p>
<p>These are inflected forms whose base form is a headword in full.
A direct lookup fails. The stemmer must find the base form automatically.
The definition screen must show the stem as the headword, not the
inflected form selected.</p>

<h2>Test A: Plural nouns not in the synonym file</h2>
<p>Select each word in the left column. Expect the definition for the
word in the right column to appear.</p>
<table>
<tr><td>pilgrims</td><td>pilgrim</td></tr>
<tr><td>ivories</td><td>ivory</td></tr>
<tr><td>quarries</td><td>quarry</td></tr>
<tr><td>timbers</td><td>timber</td></tr>
</table>

<h2>Test B: Verb inflections</h2>
<p>Select each word in the left column. Expect the definition for the
word in the right column to appear.</p>
<table>
<tr><td>yearns</td><td>yearn</td></tr>
<tr><td>yearned</td><td>yearn</td></tr>
<tr><td>harvested</td><td>harvest</td></tr>
<tr><td>harvesting</td><td>harvest</td></tr>
</table>

<h2>Test C: Comparative and superlative</h2>
<p>Select each word in the left column. Expect the definition for the
word in the right column to appear.</p>
<table>
<tr><td>narrower</td><td>narrow</td></tr>
<tr><td>narrowest</td><td>narrow</td></tr>
</table>
""")

# ---------------------------------------------------------------------------
# Ch 7 -- Synonym Search
# ---------------------------------------------------------------------------
ch("7. Synonym Search", """
<h1>7. Synonym Search</h1>

<p><strong>Setup:</strong> full selected (has a .syn file).</p>
<p>The words below are in the full synonym file but are not headwords
and cannot be reached by the stemmer. They are unrelated words, not
inflected forms. After direct lookup and all stem variants fail, the
synonym prompt must appear.</p>

<h2>Test A: Confirm path -- synonym resolves to definition</h2>
<p>Select blaze. The synonym prompt must appear. Pressing Confirm must
open the definition for flame. Repeat for each word in the table.</p>
<ul>
<li>Select blaze. Synonym prompt appears. Press Confirm.</li>
<li>Definition for flame appears.</li>
</ul>
<p>Additional words to verify the same flow:</p>
<table>
<tr><td>canopy</td><td>-> grove</td></tr>
<tr><td>coastline</td><td>-> ocean</td></tr>
<tr><td>glades</td><td>-> meadow</td></tr>
</table>

<h2>Test B: Back path -- falls through to fuzzy or not-found</h2>
<p>Select canopy. The synonym prompt must appear. Pressing Back must
skip the synonym lookup. No definition must appear. The flow must
continue to fuzzy suggestions or not-found.</p>
<ul>
<li>Select canopy. Synonym prompt appears.</li>
<li>Press Back.</li>
<li>No definition shown. Flow continues to fuzzy or not-found.</li>
</ul>

<h2>Test C: Synonym prompt then miss -- word absent from .syn</h2>
<p>Select tundra. The synonym prompt must appear because full has a .syn
file. Pressing Confirm must find no canonical entry. The flow must
fall through to fuzzy suggestions or not-found.</p>
<ul>
<li>Select tundra. Synonym prompt appears.</li>
<li>Press Confirm.</li>
<li>No canonical found. Falls through to fuzzy or not-found.</li>
</ul>
""")

# ---------------------------------------------------------------------------
# Ch 8 -- Fuzzy Suggestions
# ---------------------------------------------------------------------------
ch("8. Fuzzy Suggestions", """
<h1>8. Fuzzy Suggestions</h1>

<p><strong>Setup:</strong> full selected.</p>
<p>These are misspelled words. Because full has a .syn file, the synonym
prompt appears first -- press Confirm each time. The words are not in
.syn either, so the suggestions screen must appear next with nearby
real headwords.</p>

<h2>Test A: Suggestions list appears</h2>
<p>Select applel. The synonym prompt appears -- press Confirm. No synonym
is found. The suggestions screen must appear with nearby headwords such
as apple.</p>
<ul>
<li>Select applel.</li>
<li>Synonym prompt appears. Press Confirm.</li>
<li>Suggestions screen appears with nearby words.</li>
</ul>
<p>Additional misspellings to try:</p>
<ul>
<li>oceam</li>
<li>vally</li>
<li>shdow</li>
<li>bridqe</li>
</ul>

<h2>Test B: Select a suggestion</h2>
<p>From the suggestions screen reached in Test A, selecting a suggested
word must open its definition screen.</p>
<ul>
<li>From suggestions screen, select any suggested word.</li>
<li>Definition screen appears for that word.</li>
</ul>

<h2>Test C: Synonyms button after selecting a suggestion</h2>
<p>When arriving at a definition via the suggestions screen, the Confirm
button must be labelled Synonyms because full has a .syn file and this
path sets showDoneButton to false. Pressing Synonyms must open the
synonym definition or show not-found if none exists for that headword.</p>
<ul>
<li>On the definition screen from Test B, verify Confirm is labelled Synonyms.</li>
<li>Press Synonyms.</li>
<li>New definition screen opens, or not-found if no synonym exists.</li>
</ul>
""")

# ---------------------------------------------------------------------------
# Ch 9 -- No Matches
# ---------------------------------------------------------------------------
ch("9. No Matches", """
<h1>9. No Matches</h1>

<p><strong>Setup:</strong> full selected.</p>
<p>These strings have no dictionary entry and no close neighbours in the
index. The full failure sequence must complete without a crash and end
with the not-found popup.</p>

<h2>Test A: Full failure sequence -- Confirm at synonym prompt</h2>
<p>Select xzqwvb. Every lookup stage must fail cleanly and end at the
not-found popup. No crash at any step.</p>
<ul>
<li>Select xzqwvb.</li>
<li>Direct lookup fails, stems fail.</li>
<li>Synonym prompt appears. Press Confirm.</li>
<li>No synonym found.</li>
<li>Fuzzy search finds no neighbours.</li>
<li>Not-found popup appears: Word not found</li>
</ul>
<p>Additional strings to repeat the above sequence with:</p>
<ul>
<li>qfbrtm</li>
<li>zzzyxwv</li>
<li>blorptfq</li>
</ul>

<h2>Test B: Back at synonym prompt also reaches not-found</h2>
<p>Select qfbrtm. The synonym prompt appears. Pressing Back must still
result in the not-found popup -- the Back path must not skip the
fuzzy search.</p>
<ul>
<li>Select qfbrtm.</li>
<li>Synonym prompt appears. Press Back.</li>
<li>Fuzzy search finds no neighbours.</li>
<li>Not-found popup appears.</li>
</ul>
""")

# ---------------------------------------------------------------------------
# Ch 10 -- Edge Cases
# ---------------------------------------------------------------------------
ch("10. Edge Cases", """
<h1>10. Edge Cases</h1>

<p><strong>Setup:</strong> full selected.</p>

<h2>Test A: Back from definition returns to correct position</h2>
<p>After viewing a definition and pressing Back, the reader must return
to exactly the same page position with no layout shift.</p>
<ul>
<li>Select any word from the list below.</li>
<li>Definition screen appears.</li>
<li>Press Back.</li>
<li>Verify you are returned to this page at the same position.</li>
<li>Verify no layout shift has occurred.</li>
</ul>
<ul>
<li>apple</li>
<li>ocean</li>
<li>shadow</li>
<li>flame</li>
<li>valley</li>
</ul>

<h2>Test B: Dictionary removed mid-session</h2>
<p>When the SD card is removed and reinserted without changing the
dictionary setting, attempting a lookup must not crash the device.
Acceptable outcomes are an error popup, the dictionary UI becoming
hidden, or an automatic reset to None.</p>
<ul>
<li>With full selected and this book open, remove and reinsert the SD card,
    or power-cycle without changing the dictionary setting.</li>
<li>Attempt to look up any word from the list below.</li>
<li>Verify no crash occurs.</li>
</ul>
<ul>
<li>apple</li>
<li>cloud</li>
<li>meadow</li>
</ul>

<h2>Test C: None selection disables dictionary UI</h2>
<p>After selecting None, the dictionary word-select UI must no longer
be accessible from the reader. Selecting basic prepares for Chapter 11.</p>
<ul>
<li>Navigate to Settings -> Dictionary. Select None.</li>
<li>Return to the reader.</li>
<li>Verify the dictionary word-select UI is no longer accessible.</li>
<li>Return to Settings -> Dictionary. Select basic.</li>
</ul>
""")

# ---------------------------------------------------------------------------
# Ch 11 -- No-Syn Dictionary
# ---------------------------------------------------------------------------
ch("11. No-Syn Dictionary", """
<h1>11. No-Syn Dictionary</h1>

<p><strong>Setup:</strong> basic selected (no .syn file).</p>

<h2>Test A: No synonym prompt on lookup miss</h2>
<p>These words are not headwords in basic and are not reachable by the
stemmer. Because basic has no .syn file, the synonym prompt must not
appear. The flow must go directly to fuzzy suggestions or not-found.</p>
<ul>
<li>blaze</li>
<li>canopy</li>
<li>coastline</li>
<li>glades</li>
<li>tundra</li>
</ul>

<h2>Test B: Done button on successful lookup</h2>
<p>These words are headwords in basic. The definition screen must appear
and the Confirm button must be labelled Done, not Synonyms. This confirms
synAvailable is false when no .syn file is present.</p>
<ul>
<li>apple</li>
<li>bridge</li>
<li>cloud</li>
<li>echo</li>
<li>flame</li>
<li>grove</li>
<li>harvest</li>
<li>ocean</li>
<li>shadow</li>
<li>zenith</li>
</ul>
""")

# ---------------------------------------------------------------------------
# Ch 12 -- English to Spanish (bilingual edge case)
# ---------------------------------------------------------------------------
ch("12. English to Spanish Dictionary", """
<h1>12. English to Spanish Dictionary</h1>

<p><strong>Setup:</strong> switch to en-es. This is a bilingual English to
Spanish dictionary. Definitions are Spanish translations, not English
explanations.</p>

<h2>Test A: Dictionary info screen</h2>
<p>The info screen must show the full dictionary name and non-zero counts
for both words and synonyms.</p>
<ul>
<li>Open Settings -> Dictionary.</li>
<li>Long-press en-es to open the info screen.</li>
<li>Verify Name shows: English to Spanish</li>
<li>Verify Words count is non-zero.</li>
<li>Verify Synonyms count is non-zero.</li>
<li>Press Back. Select en-es.</li>
</ul>

<h2>Test B: Translation definitions display correctly</h2>
<p>Each word below is a headword in en-es. Selecting it must open a
definition screen showing a Spanish translation in plain text with no
rendering errors.</p>
<ul>
<li>love -- expect: amor (m) / amar</li>
<li>water -- expect: agua (f)</li>
<li>friend -- expect: amigo (m) / amiga (f)</li>
<li>fire</li>
<li>ocean</li>
<li>mountain</li>
<li>tree</li>
<li>sky</li>
</ul>

<h2>Test C: Synonym resolves to Spanish translation</h2>
<p>The words below are in the en-es synonym file but are not headwords.
After direct lookup fails, the synonym prompt must appear. Confirming
must open the Spanish translation of the canonical headword.</p>
<ul>
<li>Select auto. Synonym prompt appears. Press Confirm.</li>
<li>Definition for car appears: coche (m) / carro (m)</li>
<li>The result is a Spanish translation, confirming bilingual synonym resolution.</li>
</ul>
<p>Additional words to verify the same flow:</p>
<ul>
<li>dad -- resolves to father</li>
<li>grin -- resolves to smile</li>
</ul>
""")

# ---------------------------------------------------------------------------
# Ch 13 -- Phrase Lookup (Phase 6 -- Deferred)
# ---------------------------------------------------------------------------
ch("13. Phrase Lookup (Phase 6 -- Deferred)", """
<h1>13. Phrase Lookup (Phase 6 -- Deferred)</h1>

<p>This chapter requires Phase 6 (multi-word selection), which is not yet
implemented. The tests below are a placeholder and cannot be run until
Phase 6 is complete.</p>

<p>Two phrase test dictionaries are available:</p>
<ul>
<li>phrase -- 25 multi-word headwords (2 to 6 words), no .syn file.</li>
<li>phrase-syn -- 35 multi-word headwords with a .syn file containing
    35 alternate phrase forms (British/American variants, gerund/infinitive
    pairs, abbreviated forms).</li>
</ul>

<h2>Test A: Direct phrase lookup (phrase dictionary)</h2>
<p><strong>Setup:</strong> select phrase.</p>
<p>Using multi-word selection, select a complete phrase from the sample
text below. The definition screen must appear immediately with no
intermediate screens.</p>
<ul>
<li>Use multi-word selection to select a phrase from the sample text.</li>
<li>Definition screen appears with no synonym prompt or suggestions screen.</li>
</ul>

<h2>Test B: No synonym prompt on miss (no .syn file)</h2>
<p><strong>Setup:</strong> still on phrase.</p>
<p>Selecting a phrase not present in the dictionary must go directly to
fuzzy suggestions or not-found. The synonym prompt must not appear
because phrase has no .syn file.</p>
<ul>
<li>Select a phrase not in the dictionary.</li>
<li>Synonym prompt does not appear.</li>
<li>Falls directly to fuzzy suggestions or not-found.</li>
</ul>

<h2>Test C: Phrase synonym lookup (phrase-syn dictionary)</h2>
<p><strong>Setup:</strong> switch to phrase-syn.</p>
<p>Selecting a phrase that is in .syn but not a direct headword must
trigger the synonym prompt. Confirming must open the definition for
the canonical phrase.</p>
<ul>
<li>Select a phrase present in .syn but not in the main index.</li>
<li>Synonym prompt appears. Press Confirm.</li>
<li>Definition for the canonical phrase appears.</li>
</ul>

<h2>Sample phrase text</h2>
<p>She would bite the bullet and carry on regardless.
The decision came down to the wire.
He had to face the music after jumping the gun.
It was a blessing in disguise that turned the tide.
They burned the midnight oil trying to reinvent the wheel.</p>
""")

# ---------------------------------------------------------------------------
# EPUB packaging
# ---------------------------------------------------------------------------

def xhtml(title, body):
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">
<head><title>{title}</title></head>
<body>
{body.strip()}
</body>
</html>
"""

def opf(chapters):
    items = "\n".join(
        f'    <item id="ch{i+1}" href="ch{i+1}.xhtml" media-type="application/xhtml+xml"/>'
        for i in range(len(chapters))
    )
    spine = "\n".join(
        f'    <itemref idref="ch{i+1}"/>'
        for i in range(len(chapters))
    )
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="uid" version="2.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Dictionary Feature Tests</dc:title>
    <dc:identifier id="uid">test-dictionary-phase3</dc:identifier>
    <dc:language>en</dc:language>
    <dc:creator>CrossPoint Test Suite</dc:creator>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
{items}
  </manifest>
  <spine toc="ncx">
{spine}
  </spine>
</package>
"""

def ncx(chapters):
    points = "\n".join(
        f"""    <navPoint id="ch{i+1}" playOrder="{i+1}">
      <navLabel><text>{title}</text></navLabel>
      <content src="ch{i+1}.xhtml"/>
    </navPoint>"""
        for i, (title, _) in enumerate(chapters)
    )
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head>
    <meta name="dtb:uid" content="test-dictionary-phase3"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>Dictionary Feature Tests</text></docTitle>
  <navMap>
{points}
  </navMap>
</ncx>
"""

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf"
              media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

os.makedirs(os.path.dirname(OUT), exist_ok=True)

with zipfile.ZipFile(OUT, "w") as z:
    z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
    z.writestr("META-INF/container.xml", CONTAINER, compress_type=zipfile.ZIP_DEFLATED)
    z.writestr("OEBPS/content.opf", opf(CHAPTERS), compress_type=zipfile.ZIP_DEFLATED)
    z.writestr("OEBPS/toc.ncx", ncx(CHAPTERS), compress_type=zipfile.ZIP_DEFLATED)
    for i, (title, body) in enumerate(CHAPTERS):
        z.writestr(f"OEBPS/ch{i+1}.xhtml", xhtml(title, body),
                   compress_type=zipfile.ZIP_DEFLATED)

print(f"Written: {OUT}")
print(f"Chapters: {len(CHAPTERS)}")
for i, (t, _) in enumerate(CHAPTERS):
    print(f"  {i+1:2d}. {t}")
