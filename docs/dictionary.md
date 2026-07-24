# Dictionary

Look up words while reading an EPUB, TXT, or Markdown book using an offline StarDict dictionary stored on the SD card.

## Supported Format

The reader supports **StarDict** dictionaries. When searching for dictionaries online, look for "StarDict format" or files with `.dict`, `.idx`, and `.ifo` extensions.

A dictionary folder must contain:

- `.idx` — word index (required, **must be uncompressed** — a `.idx.gz` will not work; decompress it on your computer with `gzip -d` first)
- `.dict` or `.dict.dz` — definition data (`.dict.dz` is supported as-is; entries are decompressed on the fly during lookup)
- `.ifo` — metadata (optional)
- `.syn` — StarDict synonym aliases (optional, uncompressed, and using the same filename stem)

Not supported: compressed `.syn.gz` files, dictionaries with 64-bit index offsets (`idxoffsetbits=64` in the `.ifo` — rare, and rejected with an error), and HTML-formatted definitions render as raw markup rather than styled text. A missing or malformed optional `.syn` never disables valid direct lookups from `.idx`.

## Setting Up a Dictionary

1. Copy your dictionary folder(s) to `/dictionaries/` on the SD card — one dictionary per folder, e.g. `/dictionaries/webster/webster.idx` + `webster.dict.dz`. A hidden `/.dictionaries/` folder (dot-prefixed) works the same way, for keeping it out of the file browser.
2. Open **Settings → Reader → Dictionary** on the device.
3. Select a dictionary from the list, or **None** to disable lookups.

The Dictionary setting only appears when at least one usable dictionary folder exists. Folders containing more than one dictionary (multiple `.idx` stems) are skipped as ambiguous.

## Looking Up a Word

Two ways to start a lookup while reading an EPUB, TXT, or Markdown book:

- Open the reader menu (**Confirm**) and choose **Look Up**.
- Or set **Settings → Controls → Long-press Menu** to "Dictionary", then hold **Confirm** (~0.4s) on the reading page.

One word on the page becomes highlighted:

1. Use **Left/Right** to move between words in reading order, and the side **Up/Down** buttons to jump between lines.
2. Press **Confirm** to look up the highlighted word.
3. Press **Back** to return to the reader.

On the very first lookup with a dictionary (and again if its source files change), the reader shows *"Indexing dictionary…"* while it builds small `.qidx` and, when applicable, `.qsyn` sidecars. CrossVi samples these indexes and scans a bounded range during lookup instead of loading the complete dictionary into RAM. The sidecars can be deleted safely at any time — they will simply be rebuilt.

### How Lookup Works

1. **Direct match** — the word or selected phrase is found as-is (case-insensitive) in the dictionary index. Surrounding punctuation is ignored.
2. **Synonym match** — if present, the optional StarDict `.syn` alias points to its original `.idx` entry. A direct headword always takes priority over an alias.
3. **Stemming** — on a one-word miss, common English word forms are retried automatically: possessives and plurals (`dogs` → `dog`, `stories` → `story`) and verb endings (`walked` → `walk`, `running` → `run`, `making` → `make`).
4. **Not found** — a short popup appears and you return to word selection.

## The Definition Screen

When a word is found, the definition screen shows the matched headword at the top and the definition text below, with a page counter for long definitions.

- **Left/Right** or side **Up/Down** — previous / next page
- **Confirm** — open up to 15 successful recent lookups; selecting one looks it up again, and the final list item clears the history after confirmation
- **Back** — return to word selection

History is loaded only when opened and written atomically when leaving a reader. If the history file is malformed, too large, or unreadable, CrossVi leaves it untouched and disables history writes for that session.
