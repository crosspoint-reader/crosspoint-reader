---
name: project_dictionary_enhancement
description: Full dictionary enhancement plan — phases, decisions, HTML tag handling, all pre-coding decisions captured
type: project
---

# Dictionary Enhancement Project

## Status: Spec complete. Ready to implement Phase 1.

## Phase Order (final, agreed)
1. StarDict Infrastructure + Multi-Dictionary Picker (combined)
2. .dict.dz Decompression
3. .syn Synonym Lookup + Index Overhaul
4. HTML Definition Renderer
5. README (last, backfilled from working code)

---

## Phase 1 — StarDict Infrastructure + Multi-Dictionary Picker
- Parse .ifo metadata (bookname, wordcount, idxfilesize, synwordcount, sametypesequence)
- Folder-relative file paths (not hardcoded)
- Drop custom dictionary.cache entirely (loadCachedIndex, saveCachedIndex, CACHE_PATH)
- New DictionarySelectActivity scanning /dictionary/ subfolders for valid .ifo
- "None" as first/default option; "None found" if no valid dicts present
- User can manually select "None" to disable feature
- "View Info" sub-action: read-only display of raw .ifo file contents
- Dictionary file existence checked at every point of use (not just boot) — handle missing files gracefully, reset to "None" if selected dictionary vanishes
- Store selected path as char dictionaryPath[500] in CrossPointSettings (matches file browser)
- Add "Dictionary" ACTION entry to Reader settings tab
- Dictionary UI elements hidden when no dictionary selected (existing behavior preserved)

## Phase 2 — .dict.dz Decompression
- Detect .dict.dz present but no .dict in DictionarySelectActivity
- Validate gzip magic bytes (0x1F 0x8B) BEFORE attempting extraction
  - If invalid format: error dialog, reset selection to "None"
  - Dictionary folder remains in picker list, just deselected
- Confirmation prompt: warn extraction can take minutes
- DictDecompressActivity: progress bar, streaming via InflateReader (gzip mode)
- Add skipGzipHeader() to InflateReader — calls uzlib_gzip_parse_header()
  - ADDITIVE ONLY — no existing methods or call sites modified
- Extract .dict.dz → .dict on SD; .dict.dz retained after extraction
- Storage is user's problem (same as EPUBs)

## Phase 3 — .syn Synonym Lookup + Index Overhaul
- Replace sparse index with two-level .idx.oft index for BOTH .idx and .syn
  - L1: subsample .idx.oft every 128 entries → ~768 bytes permanent RAM
  - L2: read ~128 .idx.oft entries into ~512 byte stack buffer per lookup (does NOT accumulate)
  - L3: linear scan ~32 .idx entries
  - Read stride from .idx.oft header (never hardcode)
- lookupSynonym() via .syn two-level index
- Never automatic — always user-initiated
- "Not found" screen shows explicit "Search synonyms?" prompt
- If synonym search also finds nothing: dialog stating word was not found
- Explicit button in definition dialog for synonym lookup on current headword

## Phase 4 — HTML Definition Renderer
- New lib/DictHtmlRenderer/ using expat, independent of OpdsParser
- Produces vector<StyledSpan> {text, bold, italic, indentLevel, isListItem}

### Tag Handling (FINAL)
**Strip entirely (block + all children):**
- <svg> and children (defs, g, path, rect, use)
- <hiero> and children (eliminates all img, table, div occurrences)
- <math>
- MediaWiki extension tags: <gallery>, <nowiki>, <poem>, <ref>

**Strip tags, keep text content:**
- <span>
- All wikitext annotation tags: <lang:XX>, <gloss:XX>, <pos:XX>, <sc:XX>, <alt:XX>, <id:XX>, <t:XX>
- Unknown/unrecognized tags (default behavior)

**Special rendering:**
- <abbr>: render as "text (title attribute value)"
- <var>: italic

**Formatting:**
- <b>, <strong>: bold
- <i>, <em>: italic
- <u>: underline
- <s>: strikethrough
- <sup>, <sub>: small inline
- <code>, <tt>: monospace or plain
- <small>, <big>: plain
- <blockquote>, <poem>: indented block
- <p>, <div>, <br>: line break
- <li>: bulleted + indented
- <ol>, <ul>: block context
- <h1>-<h4>: bold

## Phase 5 — README
- Backfilled from working code
- Documents StarDict file format, SD card structure, decompression instructions
- Basic on-device usage instructions for dictionary feature

---

## Key Technical Notes
- Both provided dictionaries: sametypesequence=h (definitions are HTML)
- dict-en-en: has .dict.dz + .syn.dz (both compressed)
- dict-en-en-noetym: same structure
- .idx.oft stride: read from file, do not hardcode
- <a> tag: false positive in scan — no actual anchor tags in dictionary
- <ref>: false positive — zero actual occurrences
- All <img> are hieroglyphic glyphs (eliminated by stripping <hiero>)
- All <table> are mw-hiero-table (eliminated by stripping <hiero>)
- InflateReader: uzlib_gzip_parse_header available, just needs wrapper method
- uzlib is project code (lib/uzlib/), fully modifiable
- InflateReader is project code (lib/InflateReader/), fully modifiable
- char dictionaryPath[500] — matches file browser (FileBrowserActivity char name[500])
- Book paths use std::string (no fixed limit); CrossPointSettings uses char[] for stored strings
- Dictionary file existence must be checked at every use, not just boot

## General Approach
- Strip unknown tags by default, add support explicitly
- Phases are independent; each assumes prior phase is complete
- Additive-only changes to existing infrastructure where possible
