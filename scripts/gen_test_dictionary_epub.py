#!/usr/bin/env python3
"""
Generate test/epubs/test_dictionary.epub

Dictionary usage (4 switches after initial setup):
  Ch 1   Document Conventions
  Ch 2   Scanner validation  : no-ifo, only-dict, multi-ifo, fail-decompress
  Ch 3   Pre-processing      : gen-idx, gen-syn, extract-dict,
                               extract-syn-gen-syn, all-prep  ->  cleanup: select full
  Ch 4   Per-book override   : gen-idx (set A/B), Use Global (C), mini-prep (D), long-prep (E cancel)
  Ch 5-12  full block        : full  (26 words + 22 synonyms, English)
  Ch 13  No-syn              : basic                (switch #1)
  Ch 14  Bilingual edge case : en-es                (switch #2)
  Ch 15  Phrase [Draft]     : phrase / phrase-syn  (switch #3, final)
  Ch 16  ExpAT [Draft]

Non-ASCII / entity audit:
  &gt;    required XML escape for '>' (kept)
  &#160;  non-breaking space in '[ ]' status prefix to prevent whitespace collapse
  &#171;  left-angle guillemet in '&#171; Back' button label
  All em dashes (U+2014) replaced with --
  All arrow entities (&#8594;) replaced with ->
  Literal UTF-8: accented Spanish characters in Ch13 expected values (fully supported)
"""

import os, zipfile

OUT = os.path.join(os.path.dirname(__file__), "..", "test", "epubs", "test_dictionary.epub")

CHAPTERS = []

def ch(title, body):
    CHAPTERS.append((title, body))

# ---------------------------------------------------------------------------
# Front matter (spine only -- not in NCX navMap)
# ---------------------------------------------------------------------------

COVER_BODY = """
<h1>Dictionary Feature Tests</h1>

<p>CrossPoint Reader -- Dictionary Test Suite</p>
"""

TOC_NOTICE_BODY = """
<h1>Table of Contents</h1>

<p>Use the chapter browser to navigate this document.</p>
"""

# ---------------------------------------------------------------------------
# Ch 1 -- Document Conventions
# ---------------------------------------------------------------------------
ch("1. Document Conventions", """
<h1>1. Document Conventions</h1>

<h2>Formatting</h2>
<p>Italic text is verbatim on-screen text -- the exact words shown on the device.</p>
<ul>
<li>Select <em>full</em> in the picker. -- <em>full</em> is the dictionary name as shown</li>
<li>Press <em>Confirm</em>. -- <em>Confirm</em> is the button label as shown</li>
<li>Screen shows: <em>Preparation complete</em> -- <em>Preparation complete</em> is the status message as shown</li>
</ul>

<h2>List types</h2>
<ul>
<li>Numbered list -- ordered steps. Follow them in sequence.</li>
<li>Bulleted list -- unordered items. Try them in any order.</li>
</ul>

<h2>Test section structure</h2>
<p>Each test section may contain the following sub-headings, in this order. Sub-headings are omitted when they do not apply.</p>
<table>
<tr><td><strong>Dictionary</strong></td><td>Which dictionary to have selected for this test</td></tr>
<tr><td><strong>Note</strong></td><td>Special preconditions or warnings</td></tr>
<tr><td><strong>Steps</strong></td><td>Ordered actions to perform</td></tr>
<tr><td><strong>Words</strong></td><td>Items to look up or select</td></tr>
<tr><td><strong>Additional</strong></td><td>Extra items for extended verification</td></tr>
</table>

<h2>Processing screen</h2>
<p>Each preparation step shows a status prefix:</p>
<table>
<tr><td><em>[ &gt; ]</em></td><td>Step currently running (bold text)</td></tr>
<tr><td><em>[OK]</em></td><td>Step completed successfully</td></tr>
<tr><td><em>[!!]</em></td><td>Step failed (bold text)</td></tr>
<tr><td><em>[&#160;&#160;&#160;]</em></td><td>Step not yet started</td></tr>
</table>
<p>After all steps finish, the screen shows either <em>Preparation complete</em> or <em>Preparation failed</em>.</p>

<h2>Expected values</h2>
<p>Some entries in a word list include the definition text to verify.</p>
<ul>
<li>fire -- expect: <em>fuego (m) / incendio (m)</em></li>
</ul>
<p>The text after <em>expect:</em> must match the definition screen exactly.</p>

<h2>Draft chapters</h2>
<p>Chapters with <em>[Draft]</em> in the title document features whose firmware code does not yet exist. Those tests cannot be performed.</p>
""")

# ---------------------------------------------------------------------------
# Ch 2 -- Scanner Validation
# ---------------------------------------------------------------------------
ch("2. Dictionary Picker -- Scanner Validation", """
<h1>2. Dictionary Picker -- Scanner Validation</h1>

<p>Place all test dictionaries in /dictionary/ on the SD card.
No word lookups are performed in this chapter.
Open Settings -> Dictionary for every test below.</p>

<h2>Test A: Folder without .ifo is hidden</h2>
<p>The <em>no-ifo</em> folder must not appear in the picker list.</p>
<h3>Dictionary</h3>
<p><em>no-ifo</em></p>
<h3>Steps</h3>
<ol>
<li>Open the dictionary picker.</li>
<li>Confirm <em>no-ifo</em> is absent from the list.</li>
</ol>

<h2>Test B: Folder with only .dict is hidden</h2>
<p>The <em>only-dict</em> folder must not appear in the picker list.</p>
<h3>Dictionary</h3>
<p><em>only-dict</em></p>
<h3>Steps</h3>
<ol>
<li>Open the dictionary picker.</li>
<li>Confirm <em>only-dict</em> is absent from the list.</li>
</ol>

<h2>Test C: Folder with multiple .ifo files is hidden</h2>
<p>The <em>multi-ifo</em> folder must not appear in the picker list.</p>
<h3>Dictionary</h3>
<p><em>multi-ifo</em></p>
<h3>Steps</h3>
<ol>
<li>Open the dictionary picker.</li>
<li>Confirm <em>multi-ifo</em> is absent from the list.</li>
</ol>

<h2>Test D: All test dictionaries appear in picker</h2>
<p>All of the following must be visible in the picker. Do not select any yet.</p>
<h3>Words</h3>
<ul>
<li><em>all-prep</em></li>
<li><em>basic</em></li>
<li><em>en-es</em></li>
<li><em>extract-dict</em></li>
<li><em>extract-syn-gen-syn</em></li>
<li><em>fail-decompress</em></li>
<li><em>full</em></li>
<li><em>gen-idx</em></li>
<li><em>gen-syn</em></li>
<li><em>long-prep</em></li>
<li><em>mini-prep</em></li>
<li><em>phrase</em></li>
<li><em>phrase-syn</em></li>
</ul>
<h3>Note</h3>
<p><em>long-prep</em> and <em>mini-prep</em> are for Ch 4 per-book prepare tests only.
Do not select them from Settings.</p>

<h2>Test E: Failed decompression -- error shown, prior selection unchanged</h2>
<p>The preparation must fail with an error. The picker must remain open
and the previously selected dictionary must be unchanged. No crash.</p>
<h3>Dictionary</h3>
<p><em>fail-decompress</em></p>
<h3>Steps</h3>
<ol>
<li>Note the currently selected dictionary before starting.</li>
<li>Select <em>fail-decompress</em> in the picker.</li>
<li>The preparation screen appears listing one step: <em>Extract dictionary</em></li>
<li>Press <em>Confirm</em>.</li>
<li>Screen shows: <em>Preparation failed</em></li>
<li>Picker remains open. Previously selected dictionary is unchanged.</li>
</ol>
""")

# ---------------------------------------------------------------------------
# Ch 3 -- Pre-Processing
# ---------------------------------------------------------------------------
ch("3. Pre-Processing", """
<h1>3. Pre-Processing</h1>

<p>Each test selects a dictionary that requires one or more preparation steps.
Open Settings -> Dictionary for each test.
No word lookups are needed in this chapter.</p>

<h2>Test A: Generate dictionary offset file only</h2>
<p>Confirmation screen must list exactly one step.
That step must complete with [OK] and <em>Preparation complete</em> must appear.</p>
<h3>Dictionary</h3>
<p><em>gen-idx</em></p>
<h3>Steps</h3>
<ol>
<li>Select <em>gen-idx</em> in the picker.</li>
<li>Confirmation screen lists: <em>Generate dictionary offset file</em></li>
<li>Press <em>Confirm</em>.</li>
<li>Step completes: [OK] <em>Generate dictionary offset file</em></li>
<li><em>Preparation complete</em>.</li>
</ol>

<h2>Test B: Generate synonym offset file only</h2>
<p>Confirmation screen must list exactly one step.
That step must complete with [OK] and <em>Preparation complete</em> must appear.</p>
<h3>Dictionary</h3>
<p><em>gen-syn</em></p>
<h3>Steps</h3>
<ol>
<li>Select <em>gen-syn</em> in the picker.</li>
<li>Confirmation screen lists: <em>Generate synonym offset file</em></li>
<li>Press <em>Confirm</em>.</li>
<li>Step completes: [OK] <em>Generate synonym offset file</em></li>
<li><em>Preparation complete</em>.</li>
</ol>

<h2>Test C: Extract dictionary only</h2>
<p>Confirmation screen must list exactly one step.
That step must complete with [OK] and <em>Preparation complete</em> must appear.</p>
<h3>Dictionary</h3>
<p><em>extract-dict</em></p>
<h3>Steps</h3>
<ol>
<li>Select <em>extract-dict</em> in the picker.</li>
<li>Confirmation screen lists: <em>Extract dictionary</em></li>
<li>Press <em>Confirm</em>.</li>
<li>Step completes: [OK] <em>Extract dictionary</em></li>
<li><em>Preparation complete</em>.</li>
</ol>

<h2>Test D: Extract synonyms then generate synonym offset file</h2>
<p>Confirmation screen must list exactly two steps in order.
Each step must show in-progress bold while running, then [OK] regular
on completion. <em>Preparation complete</em> must appear after both finish.</p>
<h3>Dictionary</h3>
<p><em>extract-syn-gen-syn</em></p>
<h3>Steps</h3>
<ol>
<li>Select <em>extract-syn-gen-syn</em> in the picker.</li>
<li>Confirmation screen lists in order:
  <ul>
    <li><em>Extract synonyms</em></li>
    <li><em>Generate synonym offset file</em></li>
  </ul>
</li>
<li>Press <em>Confirm</em>.</li>
<li>Both steps run sequentially, each showing [ &gt; ] then [OK].</li>
<li><em>Preparation complete</em>.</li>
</ol>

<h2>Test E: All four preparation steps</h2>
<p>Confirmation screen must list exactly four steps in order.
Each step must complete with [OK] before the next begins.
<em>Preparation complete</em> must appear after all four finish.</p>
<h3>Dictionary</h3>
<p><em>all-prep</em></p>
<h3>Note</h3>
<p>Set the sleep timeout to 1 minute (preferred) or 5 minutes before starting
this test. The <em>all-prep</em> dictionary takes several minutes to process and the
device must not sleep during preparation or the test will be interrupted.</p>
<h3>Steps</h3>
<ol>
<li>Select <em>all-prep</em> in the picker.</li>
<li>Confirmation screen lists in order:
  <ul>
    <li><em>Extract dictionary</em></li>
    <li><em>Extract synonyms</em></li>
    <li><em>Generate dictionary offset file</em></li>
    <li><em>Generate synonym offset file</em></li>
  </ul>
</li>
<li>Press <em>Confirm</em>.</li>
<li>All four steps run sequentially.</li>
<li><em>Preparation complete</em>.</li>
</ol>

""")

# ---------------------------------------------------------------------------
# Ch 4 -- Per-Book Dictionary Override
# ---------------------------------------------------------------------------
ch("4. Per-Book Dictionary Override", """
<h1>4. Per-Book Dictionary Override</h1>

<p>The reader menu includes a <em>Book Dictionary</em> option that lets you set a
dictionary for the current book independently of the global setting in
Settings -> Dictionary. The per-book choice is saved and restored each time
the book is opened. Selecting <em>Use Global</em> clears the override and reverts
to the global setting.</p>

<h2>Test A: Set per-book dictionary from reader menu</h2>
<p>This test verifies that selecting a dictionary via the book menu overrides
the global setting and enables word lookup for this book.</p>
<h3>Dictionary</h3>
<p><em>gen-idx</em> (already prepared in Ch 3)</p>
<h3>Steps</h3>
<ol>
<li>Open the reader menu (press <em>Confirm</em>).</li>
<li>Select <em>Book Dictionary</em>.</li>
<li>The dictionary picker opens. <em>Use Global</em> is highlighted (no per-book override set yet).</li>
<li>Select <em>gen-idx</em>. The picker closes.</li>
<li>Open the reader menu again. Verify <em>Look Up Word</em> is now visible.</li>
</ol>
<h3>Words</h3>
<ul>
<li>apple</li>
<li>bridge</li>
<li>cloud</li>
</ul>

<h2>Test B: Per-book override persists after closing and reopening</h2>
<p>Closing the book and reopening it must restore the per-book dictionary
setting automatically.</p>
<h3>Steps</h3>
<ol>
<li>Press <em>&#171; Back</em> (long press) to return to the home screen.</li>
<li>Reopen this book.</li>
<li>Open the reader menu. Verify <em>Look Up Word</em> is still visible.</li>
<li>Select <em>Book Dictionary</em>. Verify <em>gen-idx</em> is marked as selected.</li>
</ol>

<h2>Test C: Clear per-book override with Use Global</h2>
<p>Selecting <em>Use Global</em> must clear the per-book override and revert lookup
availability to whatever the global setting provides.</p>
<h3>Steps</h3>
<ol>
<li>Open the reader menu -> <em>Book Dictionary</em>.</li>
<li>Select <em>Use Global</em>. The picker closes.</li>
<li>Open the reader menu. <em>Look Up Word</em> visibility now reflects the global dictionary setting.</li>
</ol>
<h3>Note</h3>
<p>If no global dictionary is set in Settings -> Dictionary, <em>Look Up Word</em>
will disappear from the menu after selecting <em>Use Global</em>.</p>

<h2>Test D: Per-book prepare -- mini-prep (all four steps, quick)</h2>
<p>Selecting an unprepared dictionary from the book menu must trigger the
same prepare workflow as selecting from Settings. All four steps must
complete and the book dictionary must be set to <em>mini-prep</em>.</p>
<h3>Dictionary</h3>
<p><em>mini-prep</em> (must not be pre-processed -- verify no .idx.oft or .syn.oft files exist)</p>
<h3>Steps</h3>
<ol>
<li>Open the reader menu -> <em>Book Dictionary</em>.</li>
<li>Select <em>mini-prep</em>.</li>
<li>Confirmation screen lists in order:
  <ul>
    <li><em>Extract dictionary</em></li>
    <li><em>Extract synonyms</em></li>
    <li><em>Generate dictionary offset file</em></li>
    <li><em>Generate synonym offset file</em></li>
  </ul>
</li>
<li>Press <em>Confirm</em>.</li>
<li>All four steps complete with [OK]. <em>Preparation complete</em>.</li>
<li>Picker closes. Open the reader menu. <em>Look Up Word</em> is visible.</li>
</ol>
<h3>Words</h3>
<ul>
<li>apple</li>
<li>cloud</li>
<li>zenith</li>
</ul>

<h2>Test E: Per-book cancel prepare -- long-prep</h2>
<p>Pressing <em>&#171; Back</em> during preparation must cancel without changing the
current per-book dictionary. The picker must return with the previous
selection still shown as active.</p>
<h3>Dictionary</h3>
<p><em>long-prep</em> (must not be pre-processed -- verify no .idx.oft or .syn.oft files exist)</p>
<h3>Note</h3>
<p>The current per-book dictionary at the start of this test is <em>mini-prep</em>
(set in Test D). Do not change it before starting.</p>
<h3>Steps</h3>
<ol>
<li>Open the reader menu -> <em>Book Dictionary</em>.</li>
<li>Select <em>long-prep</em>.</li>
<li>Confirmation screen appears listing four steps. Press <em>Confirm</em> to start.</li>
<li>Processing begins. Press <em>&#171; Back</em> to cancel during any step.</li>
<li>Screen shows: <em>User cancelled</em></li>
<li>The dictionary picker returns. <em>mini-prep</em> is still shown as selected.</li>
<li>Press <em>&#171; Back</em> to close the picker without changing the book dictionary.</li>
</ol>
""")

# ---------------------------------------------------------------------------
# Ch 5 -- Direct Lookup
# ---------------------------------------------------------------------------
ch("5. Direct Lookup", """
<h1>5. Direct Lookup</h1>

<p>These tests cover the basic lookup path: a headword found directly in the
index returns a definition immediately. No stemming, synonym prompt, or
suggestions screen is involved.</p>
<h3>Dictionary</h3>
<p><em>full</em></p>

<h2>Test A: Definition returned immediately</h2>
<p>Each word below is a headword in <em>full</em>. Selecting it must open the
definition screen directly with no synonym prompt or suggestions screen.</p>
<h3>Words</h3>
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
<p>After any successful lookup from Test A, the <em>Confirm</em> button on the
definition screen must be labelled <em>Done</em>. This confirms showDoneButton
is set correctly for direct hits.</p>
<h3>Steps</h3>
<ol>
<li>Look up any word from Test A.</li>
<li>Verify <em>Confirm</em> button label is <em>Done</em>, not <em>Synonyms</em>.</li>
</ol>
""")

# ---------------------------------------------------------------------------
# Ch 6 -- Case Normalization
# ---------------------------------------------------------------------------
ch("6. Case Normalization", """
<h1>6. Case Normalization</h1>

<p>The lookup must normalise to lowercase before searching the index.
Each capitalised word must resolve to a definition with no not-found
message and no synonym prompt.</p>
<h3>Dictionary</h3>
<p><em>full</em></p>

<h2>Test A: Capitalised sentence-initial words resolve correctly</h2>
<p>Each word below appears capitalised in the sample text. Select it from the passage.</p>
<h3>Words</h3>
<ul>
<li>Ocean</li>
<li>Shadow</li>
<li>Flame</li>
<li>Dawn</li>
<li>Meadow</li>
<li>Harvest</li>
<li>Willow</li>
</ul>
<h3>Additional</h3>
<p>Sample text containing the words above:</p>
<p>Ocean tides rise and fall each day.
Shadow falls across the valley floor.
Flame leaps high in the still night air.
Dawn breaks slowly over the distant ridge.
Meadow grasses bend gently in the wind.
Harvest time brings the whole village together.
Willow branches trail softly in the stream.</p>
""")

# ---------------------------------------------------------------------------
# Ch 7 -- Index Boundaries
# ---------------------------------------------------------------------------
ch("7. Index Boundaries", """
<h1>7. Index Boundaries</h1>

<p>These words sit at the very start and end of the <em>full</em> index,
exercising the binary search at its boundaries. Each must return
a definition without errors.</p>
<h3>Dictionary</h3>
<p><em>full</em></p>

<h2>Test A: Words at the start of the index</h2>
<h3>Words</h3>
<ul>
<li>apple</li>
<li>bridge</li>
<li>cloud</li>
<li>dawn</li>
<li>echo</li>
</ul>

<h2>Test B: Words at the end of the index</h2>
<h3>Words</h3>
<ul>
<li>xenon</li>
<li>yearn</li>
<li>zenith</li>
</ul>
""")

# ---------------------------------------------------------------------------
# Ch 8 -- Stem Variants
# ---------------------------------------------------------------------------
ch("8. Stem Variants", """
<h1>8. Stem Variants</h1>

<p>These are inflected forms whose base form is a headword in <em>full</em>.
A direct lookup fails. The stemmer must find the base form automatically.
The definition screen must show the stem as the headword, not the
inflected form selected.</p>
<h3>Dictionary</h3>
<p><em>full</em></p>

<h2>Test A: Plural nouns not in the synonym file</h2>
<p>Select each word in the left column. Expect the definition for the
word in the right column to appear.</p>
<h3>Words</h3>
<table>
<tr><td>pilgrims</td><td>pilgrim</td></tr>
<tr><td>ivories</td><td>ivory</td></tr>
<tr><td>quarries</td><td>quarry</td></tr>
<tr><td>timbers</td><td>timber</td></tr>
</table>

<h2>Test B: Verb inflections</h2>
<p>Select each word in the left column. Expect the definition for the
word in the right column to appear.</p>
<h3>Words</h3>
<table>
<tr><td>yearns</td><td>yearn</td></tr>
<tr><td>yearned</td><td>yearn</td></tr>
<tr><td>harvested</td><td>harvest</td></tr>
<tr><td>harvesting</td><td>harvest</td></tr>
</table>

<h2>Test C: Comparative and superlative</h2>
<p>Select each word in the left column. Expect the definition for the
word in the right column to appear.</p>
<h3>Words</h3>
<table>
<tr><td>narrower</td><td>narrow</td></tr>
<tr><td>narrowest</td><td>narrow</td></tr>
</table>
""")

# ---------------------------------------------------------------------------
# Ch 9 -- Synonym Search
# ---------------------------------------------------------------------------
ch("9. Synonym Search", """
<h1>9. Synonym Search</h1>

<p>The words below are in the <em>full</em> synonym file but are not headwords
and cannot be reached by the stemmer. They are unrelated words, not
inflected forms. After direct lookup and all stem variants fail, the
synonym prompt must appear.</p>
<h3>Dictionary</h3>
<p><em>full</em></p>

<h2>Test A: Confirm path -- synonym resolves to definition</h2>
<p>Select each word in the left column. The synonym prompt must appear.
Press <em>Confirm</em>. The definition for the word in the right column must open.</p>
<h3>Words</h3>
<table>
<tr><td>blaze</td><td>-> flame</td></tr>
<tr><td>canopy</td><td>-> grove</td></tr>
<tr><td>coastline</td><td>-> ocean</td></tr>
<tr><td>glades</td><td>-> meadow</td></tr>
</table>

<h2>Test B: Back path -- falls through to fuzzy or not-found</h2>
<p>The synonym prompt must appear. Pressing <em>&#171; Back</em> must skip the
synonym lookup. No definition must appear. The flow must continue to
fuzzy suggestions or not-found.</p>
<h3>Steps</h3>
<ol>
<li>Select canopy. Synonym prompt appears.</li>
<li>Press <em>&#171; Back</em>.</li>
<li>No definition shown. Flow continues to fuzzy or not-found.</li>
</ol>

<h2>Test C: Synonym prompt then miss -- word absent from .syn</h2>
<p>The synonym prompt must appear because <em>full</em> has a .syn file.
Pressing <em>Confirm</em> must find no canonical entry. The flow must fall
through to fuzzy suggestions or not-found.</p>
<h3>Steps</h3>
<ol>
<li>Select tundra. Synonym prompt appears.</li>
<li>Press <em>Confirm</em>.</li>
<li>No canonical found. Falls through to fuzzy or not-found.</li>
</ol>
""")

# ---------------------------------------------------------------------------
# Ch 10 -- Fuzzy Suggestions
# ---------------------------------------------------------------------------
ch("10. Fuzzy Suggestions", """
<h1>10. Fuzzy Suggestions</h1>

<p>These are misspelled words. Because <em>full</em> has a .syn file, the synonym
prompt appears first -- press <em>Confirm</em> each time. The words are not in
.syn either, so the suggestions screen must appear next with nearby
real headwords.</p>
<h3>Dictionary</h3>
<p><em>full</em></p>

<h2>Test A: Suggestions list appears</h2>
<p>Select a misspelled word. The synonym prompt appears -- press <em>Confirm</em>.
No synonym is found. The suggestions screen must appear with nearby headwords.</p>
<h3>Steps</h3>
<ol>
<li>Select applel.</li>
<li>Synonym prompt appears. Press <em>Confirm</em>.</li>
<li>Suggestions screen appears with nearby words.</li>
</ol>
<h3>Additional</h3>
<ul>
<li>oceam</li>
<li>vally</li>
<li>shdow</li>
<li>bridqe</li>
</ul>

<h2>Test B: Select a suggestion</h2>
<p>From the suggestions screen reached in Test A, selecting a suggested
word must open its definition screen.</p>
<h3>Steps</h3>
<ol>
<li>From suggestions screen, select any suggested word.</li>
<li>Definition screen appears for that word.</li>
</ol>

<h2>Test C: Synonyms button after selecting a suggestion</h2>
<p>When arriving at a definition via the suggestions screen, the <em>Confirm</em>
button must be labelled <em>Synonyms</em> because <em>full</em> has a .syn file and this
path sets showDoneButton to false. Pressing <em>Synonyms</em> must open the
synonym definition or show not-found if none exists for that headword.</p>
<h3>Steps</h3>
<ol>
<li>On the definition screen from Test B, verify <em>Confirm</em> is labelled <em>Synonyms</em>.</li>
<li>Press <em>Synonyms</em>.</li>
<li>New definition screen opens, or not-found if no synonym exists.</li>
</ol>
""")

# ---------------------------------------------------------------------------
# Ch 11 -- No Matches
# ---------------------------------------------------------------------------
ch("11. No Matches", """
<h1>11. No Matches</h1>

<p>These strings have no dictionary entry and no close neighbours in the
index. The full failure sequence must complete without a crash and end
with the not-found popup.</p>
<h3>Dictionary</h3>
<p><em>full</em></p>

<h2>Test A: Full failure sequence -- Confirm at synonym prompt</h2>
<p>Every lookup stage must fail cleanly and end at the not-found popup.
No crash at any step.</p>
<h3>Steps</h3>
<ol>
<li>Select xzqwvb.</li>
<li>Direct lookup fails, stems fail.</li>
<li>Synonym prompt appears. Press <em>Confirm</em>.</li>
<li>No synonym found.</li>
<li>Fuzzy search finds no neighbours.</li>
<li>Not-found popup appears: <em>Word not found</em></li>
</ol>
<h3>Additional</h3>
<ul>
<li>qfbrtm</li>
<li>zzzyxwv</li>
<li>blorptfq</li>
</ul>

<h2>Test B: Back at synonym prompt also reaches not-found</h2>
<p>The synonym prompt appears. Pressing <em>&#171; Back</em> must still result in the
not-found popup -- the <em>&#171; Back</em> path must not skip the fuzzy search.</p>
<h3>Steps</h3>
<ol>
<li>Select qfbrtm.</li>
<li>Synonym prompt appears. Press <em>&#171; Back</em>.</li>
<li>Fuzzy search finds no neighbours.</li>
<li>Not-found popup appears.</li>
</ol>
""")

# ---------------------------------------------------------------------------
# Ch 12 -- Edge Cases
# ---------------------------------------------------------------------------
ch("12. Edge Cases", """
<h1>12. Edge Cases</h1>

<p>These tests cover back navigation, SD card state changes, and disabling
the dictionary.</p>
<h3>Dictionary</h3>
<p><em>full</em></p>

<h2>Test A: Back from definition returns to correct position</h2>
<p>After viewing a definition and pressing <em>&#171; Back</em>, the reader must return
to exactly the same page position with no layout shift.</p>
<h3>Steps</h3>
<ol>
<li>Select any word from the list below.</li>
<li>Definition screen appears.</li>
<li>Press <em>&#171; Back</em>.</li>
<li>Verify you are returned to this page at the same position.</li>
<li>Verify no layout shift has occurred.</li>
</ol>
<h3>Words</h3>
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
<h3>Steps</h3>
<ol>
<li>With <em>full</em> selected and this book open, remove and reinsert the SD card,
    or power-cycle without changing the dictionary setting.</li>
<li>Attempt to look up any word from the list below.</li>
<li>Verify no crash occurs.</li>
</ol>
<h3>Words</h3>
<ul>
<li>apple</li>
<li>cloud</li>
<li>meadow</li>
</ul>

<h2>Test C: None selection disables dictionary UI</h2>
<p>After selecting None, the dictionary word-select UI must no longer
be accessible from the reader. Selecting <em>basic</em> prepares for Chapter 13.</p>
<h3>Steps</h3>
<ol>
<li>Navigate to Settings -> Dictionary. Select None.</li>
<li>Return to the reader.</li>
<li>Verify the dictionary word-select UI is no longer accessible.</li>
<li>Return to Settings -> Dictionary. Select <em>basic</em>.</li>
</ol>
""")

# ---------------------------------------------------------------------------
# Ch 13 -- No-Syn Dictionary
# ---------------------------------------------------------------------------
ch("13. No-Syn Dictionary", """
<h1>13. No-Syn Dictionary</h1>

<p><em>basic</em> has no .syn file. These tests verify the synonym prompt does
not appear when no synonym file is present.</p>
<h3>Dictionary</h3>
<p><em>basic</em></p>

<h2>Test A: No synonym prompt on lookup miss</h2>
<p>These words are not headwords in <em>basic</em> and are not reachable by the
stemmer. Because <em>basic</em> has no .syn file, the synonym prompt must not
appear. The flow must go directly to fuzzy suggestions or not-found.</p>
<h3>Words</h3>
<ul>
<li>blaze</li>
<li>canopy</li>
<li>coastline</li>
<li>glades</li>
<li>tundra</li>
</ul>

<h2>Test B: Done button on successful lookup</h2>
<p>These words are headwords in <em>basic</em>. The definition screen must appear
and the <em>Confirm</em> button must be labelled <em>Done</em>, not <em>Synonyms</em>. This confirms
synAvailable is false when no .syn file is present.</p>
<h3>Words</h3>
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
# Ch 14 -- English to Spanish (bilingual edge case)
# ---------------------------------------------------------------------------
ch("14. English to Spanish Dictionary", """
<h1>14. English to Spanish Dictionary</h1>

<p>This is a bilingual English to Spanish dictionary. Definitions are
Spanish translations, not English explanations.</p>
<h3>Dictionary</h3>
<p><em>en-es</em></p>

<h2>Test A: Dictionary info screen</h2>
<p>The info screen must show the full dictionary name and non-zero counts
for both words and synonyms.</p>
<h3>Steps</h3>
<ol>
<li>Open Settings -> Dictionary.</li>
<li>Long-press <em>en-es</em> to open the info screen.</li>
<li>Verify Name shows: <em>English to Spanish</em></li>
<li>Verify Words count is non-zero.</li>
<li>Verify Synonyms count is non-zero.</li>
<li>Press <em>&#171; Back</em>. Select <em>en-es</em>.</li>
</ol>

<h2>Test B: Translation definitions display correctly</h2>
<p>Each word below is a headword in <em>en-es</em>. Selecting it must open a
definition screen showing a Spanish translation in plain text with no
rendering errors.</p>
<h3>Words</h3>
<ul>
<li>love -- expect: <em>amor (m) / amar</em></li>
<li>water -- expect: <em>agua (f)</em></li>
<li>friend -- expect: <em>amigo (m) / amiga (f)</em></li>
<li>fire -- expect: <em>fuego (m) / incendio (m)</em></li>
<li>ocean -- expect: <em>océano (m)</em></li>
<li>mountain -- expect: <em>montaña (f)</em></li>
<li>tree -- expect: <em>árbol (m)</em></li>
<li>sky -- expect: <em>cielo (m)</em></li>
</ul>

<h2>Test C: Synonym resolves to Spanish translation</h2>
<p>The words below are in the <em>en-es</em> synonym file but are not headwords.
After direct lookup fails, the synonym prompt must appear. Confirming
must open the Spanish translation of the canonical headword.</p>
<h3>Steps</h3>
<ol>
<li>Select auto. Synonym prompt appears. Press <em>Confirm</em>.</li>
<li>Definition for car appears: <em>coche (m) / carro (m)</em></li>
<li>The result is a Spanish translation, confirming bilingual synonym resolution.</li>
</ol>
<h3>Additional</h3>
<ul>
<li>dad -> father</li>
<li>grin -> smile</li>
</ul>
""")

# ---------------------------------------------------------------------------
# Ch 15 -- Phrase Lookup [Draft]
# ---------------------------------------------------------------------------
ch("15. Phrase Lookup [Draft]", """
<h1>15. Phrase Lookup [Draft]</h1>

<p><strong>Not implemented:</strong> The firmware code supporting this chapter does not exist yet. These tests cannot be performed.</p>

<p><strong><em>DRAFT: This chapter is incomplete and subject to change.</em></strong></p>

<p>This chapter requires Phase 6 (multi-word selection), which is not yet
implemented. The tests below are a placeholder and cannot be run until
Phase 6 is complete.</p>

<p>Two phrase test dictionaries are available:</p>
<ul>
<li><em>phrase</em> -- 25 multi-word headwords (2 to 6 words), no .syn file.</li>
<li><em>phrase-syn</em> -- 35 multi-word headwords with a .syn file containing
    35 alternate phrase forms (British/American variants, gerund/infinitive
    pairs, abbreviated forms).</li>
</ul>

<h2>Test A: Direct phrase lookup</h2>
<p>Using multi-word selection, select a complete phrase from the sample
text below. The definition screen must appear immediately with no
intermediate screens.</p>
<h3>Dictionary</h3>
<p><em>phrase</em></p>
<h3>Steps</h3>
<ol>
<li>Use multi-word selection to select a phrase from the sample text.</li>
<li>Definition screen appears with no synonym prompt or suggestions screen.</li>
</ol>

<h2>Test B: No synonym prompt on miss (no .syn file)</h2>
<p>Selecting a phrase not present in the dictionary must go directly to
fuzzy suggestions or not-found. The synonym prompt must not appear
because <em>phrase</em> has no .syn file.</p>
<h3>Dictionary</h3>
<p><em>phrase</em></p>
<h3>Steps</h3>
<ol>
<li>Select a phrase not in the dictionary.</li>
<li>Synonym prompt does not appear.</li>
<li>Falls directly to fuzzy suggestions or not-found.</li>
</ol>

<h2>Test C: Phrase synonym lookup</h2>
<p>Selecting a phrase that is in .syn but not a direct headword must
trigger the synonym prompt. Confirming must open the definition for
the canonical phrase.</p>
<h3>Dictionary</h3>
<p><em>phrase-syn</em></p>
<h3>Steps</h3>
<ol>
<li>Select a phrase present in .syn but not in the main index.</li>
<li>Synonym prompt appears. Press <em>Confirm</em>.</li>
<li>Definition for the canonical phrase appears.</li>
</ol>

<h2>Sample phrase text</h2>
<p>She would bite the bullet and carry on regardless.
The decision came down to the wire.
He had to face the music after jumping the gun.
It was a blessing in disguise that turned the tide.
They burned the midnight oil trying to reinvent the wheel.</p>
""")

# ---------------------------------------------------------------------------
# Ch 16 -- ExpAT HTML Renderer Tests [Draft]
# ---------------------------------------------------------------------------
ch("16. ExpAT HTML Renderer Tests [Draft]", """
<h1>16. ExpAT HTML Renderer Tests [Draft]</h1>

<p><strong>Not implemented:</strong> The firmware code supporting this chapter does not exist yet. These tests cannot be performed.</p>

<p><strong><em>DRAFT: This chapter is incomplete and subject to change.</em></strong></p>

<p>This chapter tests the expat-based HTML renderer used to display dictionary
definitions. Set the active dictionary to <em>expat-full</em> before starting. Each test
names a synthetic word to look up. Verify the definition screen matches the
description shown at the top of the definition.</p>

<p>Do not use <em>expat-full</em> for any other chapter in this book.</p>

<h2>Category 1: Strip Entirely (8 tests)</h2>
<p>Each definition should show the description followed by two rules with
nothing between them. If any text appears between the rules, the tag is not
being stripped correctly.</p>
<ul>
<li><strong>BlazeSilent</strong> -- hiero block</li>
<li><strong>ClearSvg</strong> -- svg block</li>
<li><strong>DarkMath</strong> -- math block</li>
<li><strong>EmptyGallery</strong> -- gallery tag</li>
<li><strong>FrostNowiki</strong> -- nowiki tag</li>
<li><strong>GlowPoem</strong> -- poem tag</li>
<li><strong>HazeRef</strong> -- ref tag (lowercase)</li>
<li><strong>IcyUpper</strong> -- REF tag (uppercase)</li>
</ul>

<h2>Category 2: Formatting (12 tests)</h2>
<p>Each definition shows the expected formatting between the rules.</p>
<ul>
<li><strong>JadeBold</strong> -- b element: word appears bold</li>
<li><strong>KelpItalic</strong> -- i element: word appears italic</li>
<li><strong>LimeSub</strong> -- sub element: subscript digit in H2O</li>
<li><strong>MistSup</strong> -- sup element: superscript digit in x2</li>
<li><strong>NightUnder</strong> -- u element: word appears underlined</li>
<li><strong>OakStrike</strong> -- s element: word appears with strikethrough</li>
<li><strong>PineCode</strong> -- code element: text appears in code style</li>
<li><strong>QuartzSmall</strong> -- small element: word appears small</li>
<li><strong>ReedBig</strong> -- big element: word appears big</li>
<li><strong>SageVar</strong> -- var element: variable name appears italic</li>
<li><strong>TealNested</strong> -- nested b and i: three words with different formatting</li>
<li><strong>UmberDeep</strong> -- nested b, i, and u: four words with increasing formatting</li>
</ul>

<h2>Category 3: Block Structure (10 tests)</h2>
<p>Each definition shows the expected block layout between the rules.</p>
<ul>
<li><strong>AlumList</strong> -- ul+li: bulleted list of three items</li>
<li><strong>BronzeHead1</strong> -- h1: bold heading</li>
<li><strong>CopperHead2</strong> -- h2: bold heading</li>
<li><strong>DuskHead3</strong> -- h3: bold heading</li>
<li><strong>ElmHead4</strong> -- h4: bold heading</li>
<li><strong>VinePara</strong> -- p: two paragraphs separated by blank line</li>
<li><strong>WildDiv</strong> -- div: two blocks separated by line break</li>
<li><strong>XenBreak</strong> -- br: three lines on separate lines</li>
<li><strong>YewQuote</strong> -- blockquote: indented passage</li>
<li><strong>ZincOrder</strong> -- ol+li: numbered list of three items</li>
</ul>

<h2>Category 4: Wikitext Annotations (8 tests)</h2>
<p>Each definition shows text extracted from the tag name suffix of a
self-closing wikitext annotation tag. The tag itself should not appear.</p>
<ul>
<li><strong>FernTranslate</strong> -- t: tag: word "four" appears inline</li>
<li><strong>GoldTranslit</strong> -- tr: tag: text "oikos" appears inline</li>
<li><strong>HullLang</strong> -- lang: tag: text "la" appears inline</li>
<li><strong>IronGloss</strong> -- gloss: tag: text "sharp" appears inline</li>
<li><strong>JetPos</strong> -- pos: tag: text "noun" appears inline</li>
<li><strong>KobaltScript</strong> -- sc: tag: text "Grek" appears inline</li>
<li><strong>LarchAlt</strong> -- alt: tag: text "ameba" appears inline</li>
<li><strong>MapleId</strong> -- id: tag: text "female" appears inline</li>
</ul>

<h2>Category 5: Abbr Title Expansion (3 tests)</h2>
<p>Each definition shows the abbr text followed by the title in parentheses.</p>
<ul>
<li><strong>NebulAbbr</strong> -- basic: "c. (circa)"</li>
<li><strong>OakAbbrEntity</strong> -- multi-word title: "AD (anno Domini)"</li>
<li><strong>PearlAbbrNest</strong> -- italic inside abbr: "f. (filius)" with f. italic</li>
</ul>

<h2>Category 6: Strip-Tag Keep-Text (3 tests)</h2>
<p>Each definition shows the text content of the stripped tag between the rules.
The tag itself should not appear, only its text content.</p>
<ul>
<li><strong>QuillUnknown</strong> -- single unknown tag: "visible text" appears</li>
<li><strong>RubyNested</strong> -- nested unknown tags: "inner text" appears</li>
<li><strong>SteelSpan</strong> -- span element: "visible span" appears</li>
</ul>
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
    <item id="cover" href="cover.xhtml" media-type="application/xhtml+xml"/>
    <item id="toc-notice" href="toc-notice.xhtml" media-type="application/xhtml+xml"/>
{items}
  </manifest>
  <spine toc="ncx">
    <itemref idref="cover"/>
    <itemref idref="toc-notice"/>
{spine}
  </spine>
  <guide>
    <reference type="text" href="ch1.xhtml" title="Begin"/>
  </guide>
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
    z.writestr("OEBPS/cover.xhtml", xhtml("Dictionary Feature Tests", COVER_BODY),
               compress_type=zipfile.ZIP_DEFLATED)
    z.writestr("OEBPS/toc-notice.xhtml", xhtml("Table of Contents", TOC_NOTICE_BODY),
               compress_type=zipfile.ZIP_DEFLATED)
    for i, (title, body) in enumerate(CHAPTERS):
        z.writestr(f"OEBPS/ch{i+1}.xhtml", xhtml(title, body),
                   compress_type=zipfile.ZIP_DEFLATED)

print(f"Written: {OUT}")
print(f"Front matter: cover.xhtml, toc-notice.xhtml (spine only, not in chapter browser)")
print(f"Chapters: {len(CHAPTERS)}")
for i, (t, _) in enumerate(CHAPTERS):
    print(f"  {i+1:2d}. {t}")
