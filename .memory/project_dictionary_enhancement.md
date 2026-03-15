---
name: project_dictionary_enhancement
description: Full dictionary enhancement plan — phases, decisions, tag handling, all pre-coding decisions captured
type: project
---

# Dictionary Enhancement Project

## Status: Pre-coding. All spec/design decisions finalized. Ready to implement.

## Phase Order (agreed)
1. StarDict Infrastructure Refactor
2. Multi-Dictionary Settings + Picker
3. .dict.dz Decompression
4. .syn Synonym Lookup (user-initiated)
5. HTML Definition Renderer
6. README (last, backfilled from working code)

## Phase 1 — StarDict Infrastructure Refactor
- Parse .ifo metadata (bookname, wordcount, idxfilesize, synwordcount, sametypesequence)
- Replace hardcoded `/dictionary/dictionary.*` paths with folder-relative path construction
- Drop custom cache entirely (loadCachedIndex, saveCachedIndex, CACHE_PATH, dictionary.cache)
- Use .idx.oft natively as the two-level index:
  - L1: subsample .idx.oft every 128 entries → ~768 bytes permanent RAM
  - L2: read ~128 .idx.oft entries from SD into ~512 byte stack buffer per lookup
  - L3: linear scan ~32 .idx entries
- Build parallel two-level index for .syn/.syn.oft (same format)
- Read stride value from .idx.oft file header (do not hardcode 32)

## Phase 2 — Multi-Dictionary Settings + Picker
- New DictionarySelectActivity: scans /dictionary/ subdirs for valid .ifo
- List shows folder names; "None" is first option; "None found" if no valid dicts present
- "View Info" sub-action: read-only display of raw .ifo file contents
- Validation on boot/entry: if previously selected folder no longer exists → reset to "None"
- User can manually select "None" to disable dictionary feature
- Store selected path as char dictionaryPath[64] in CrossPointSettings
- Add "Dictionary" ACTION entry to Reader settings tab
- Dictionary UI elements (lookup menu items) hidden when no dictionary selected — same as current behavior

## Phase 3 — .dict.dz Decompression
- Detect .dict.dz present but no .dict in DictionarySelectActivity
- Validate gzip magic bytes (0x1F 0x8B) BEFORE attempting extraction
  - If invalid format: show error dialog, reset active dictionary to "none"
  - Dictionary folder remains in picker list, just deselected
- Confirmation prompt: warn extraction can take minutes
- DictDecompressActivity: progress bar, streaming via InflateReader (gzip mode)
- Add skipGzipHeader() to InflateReader — calls uzlib_gzip_parse_header()
  - ADDITIVE ONLY — no existing methods or call sites modified
- Extract .dict.dz → .dict on SD; .dict.dz retained after extraction
- Storage is user's problem (same as EPUBs)

## Phase 4 — .syn Synonym Lookup (User-Initiated)
- lookupSynonym(word): binary search .syn via two-level index → wordIndex → navigate .idx → read definition
- Lookup chain: lookup() → [not found] → show "Not found" with explicit "Search synonyms?" prompt
- In DictionaryDefinitionActivity: explicit button to trigger synonym lookup on current headword
- No automatic synonym searching — always user-initiated

## Phase 5 — HTML Definition Renderer
- New lib/DictHtmlRenderer/: standalone expat processor, independent of OpdsParser
- Produces vector<StyledSpan> {text, bold, italic, indentLevel, isListItem}

### Tag Handling Decisions (FINAL)
**Strip entirely (block + all children):**
- <svg> and children (defs, g, path, rect, use)
- <hiero> and children (eliminates all img, table, div occurrences)
- <math>
- All MediaWiki extension tags: <gallery>, <nowiki>, <poem>, <ref>

**Strip tags, keep text content:**
- <span>
- All wikitext annotation tags: <lang:XX>, <gloss:XX>, <pos:XX>, <sc:XX>, <alt:XX>, <id:XX>, <t:XX>
- Unknown/unrecognized tags (default behavior)

**Special rendering:**
- <abbr>: render as "text (title attribute value)" — extract title, append in parens
- <var>: render as italic

**Formatting:**
- <b>, <strong>: bold
- <i>, <em>: italic
- <u>: underline
- <s>: strikethrough
- <sup>, <sub>: small inline
- <code>, <tt>: monospace or plain
- <small>, <big>: plain (no font size control on e-ink)
- <blockquote>, <poem>: indented block
- <p>, <div>, <br>: line break
- <li>: indented with bullet prefix
- <ol>, <ul>: block context for list items
- <h1>-<h4>: bold

## Key Technical Notes
- Both provided dictionaries: sametypesequence=h (definitions are HTML)
- dict-en-en: has .dict.dz + .syn.dz (both compressed)
- dict-en-en-noetym: same structure
- .idx.oft stride: read from file, do not hardcode
- <a> tag: false positive in scan — no actual anchor tags in dictionary
- <ref>: false positive — zero actual occurrences
- All <img> are hieroglyphic glyphs (eliminated by stripping <hiero>)
- All <table> are mw-hiero-table (eliminated by stripping <hiero>)
- InflateReader: has uzlib_gzip_parse_header available, just needs wrapper method
- uzlib is project code (lib/uzlib/), fully modifiable
- InflateReader is project code (lib/InflateReader/), fully modifiable

## General Approach
- Strip unknown tags by default, add support explicitly
- Default to "remove support" over "add support" — easier to add than remove
