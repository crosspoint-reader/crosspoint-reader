# Dictionary

*Ported from [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader), where the feature and this document originate: PR #2583 by Uri Tauber, with contributions from William Floyd, Thiago Kenji Okada, Justin Mitchell, Ryan Hitchman, Kurtis Grant, kemonine and DustinHu. Adapted here for this device — see the notes on background work, fonts and dictionary switching below.*

Look up words while reading an EPUB using an offline StarDict dictionary stored on the SD card.

## Supported Format

The reader supports **StarDict** dictionaries. When searching for dictionaries online, look for "StarDict format" or files with `.dict`, `.idx`, and `.ifo` extensions.

A dictionary folder must contain:

- `.idx` — word index (required, **must be uncompressed** — a `.idx.gz` will not work; decompress it on your computer with `gzip -d` first)
- `.dict` or `.dict.dz` — definition data (`.dict.dz` is supported as-is; entries are decompressed on the fly during lookup)
- `.syn` — synonym index (optional; maps alternate spellings and irregular forms to their headword)
- `.ifo` — metadata (optional)

Not supported: dictionaries with 64-bit index offsets (`idxoffsetbits=64` in the `.ifo` — rare, and rejected with an error).


## Setting Up a Dictionary

1. Copy your dictionary folder(s) to `/dictionaries/` on the SD card — one dictionary per folder, e.g. `/dictionaries/webster/webster.idx` + `webster.dict.dz`. A hidden `/.dictionaries/` folder (dot-prefixed) works the same way, for keeping it out of the file browser.
2. Open **Settings → Reader → Dictionary** on the device.
3. Select a dictionary from the list, or **None** to disable lookups.

Keep the folder name to 31 characters or fewer: the reader remembers the chosen dictionary by its folder name and keeps no more than that, so a longer one cannot be selected.

Folders containing more than one dictionary (multiple `.idx` stems) are skipped as ambiguous, as are folders holding an `.idx` with no matching `.dict`/`.dict.dz` next to it -- a dictionary that would only fail once you tried to use it.

If you can, decompress `.dict.dz` to `.dict` on your computer (`dictunzip yourdict.dict.dz`). A `.dict.dz` lookup needs a 32 KB contiguous inflate window, and that is the one allocation on this path a fragmented reading heap cannot always supply; a plain `.dict` needs none of it and is read directly.

## Install from the Web Interface

Start **File Transfer** on the device, open its address in a browser and go to **Dictionaries** (`http://<device-ip>/dictionary`). The page does the preparation above for you:

- Choose the archive you downloaded (`.zip`, `.tar.gz`, `.tgz`, `.tar.bz2` or `.tar`) or the dictionary's own files (`.ifo`, `.idx` or `.idx.gz`, `.dict` or `.dict.dz`, optional `.syn`), or drop them on the page. Other archive formats (`.tar.xz`, `.7z`, `.rar`) are refused: repack them as `.zip`.
- Your browser unpacks `.idx.gz` and `.syn.gz`, and offers to unpack `.dict.dz` to `.dict` (on by default, showing the size it will have). A dictionary with `idxoffsetbits=64`, or without an index or data file, is refused with the reason.
- Each dictionary goes into its own folder under `/dictionaries`, named after the title in its `.ifo` (or the archive), shortened to 31 characters — you can change the name before installing. An archive holding several dictionaries is split into one folder each. A folder that already exists is replaced only after you confirm.
- The list of installed dictionaries shows each one's title, word count, files and whether the reader can use it, and **Remove** deletes a dictionary's folder.

Then choose the dictionary on the device as usual (**Settings → Reader → Dictionary**). Unpacking happens in the browser, so a large dictionary needs a current Chrome, Edge, Firefox or Safari and some patience while it uploads; a `.tar.bz2` is decoded more slowly than the other formats.

## Looking Up a Word

Two ways to start a lookup while reading:

- Open the reader menu (**Confirm**) and choose **Dictionary**.
- Or map any button press to the **Dictionary** action under **Settings → Controls**.

Choosing it with no dictionary selected opens the picker rather than reporting the problem, so the setup above can be done from the book.

While a lookup is open the reader stops all of its background work — the look-ahead section build and the next-page pre-render — and hands back the memory they were holding: the 48 KB region the look-ahead borrows, the build arena inside it, and the pre-rendered next page. A lookup needs a large contiguous block at the point in a reading session where the heap is at its most fragmented, and nothing else should be competing for it.

No completed work is thrown away. A look-ahead build that was still running is aborted, but it keeps whatever it had already extracted, so the retry after the lookup skips that step; a build that had finished is left alone, because its result is the cache file on the card rather than anything held in RAM. Background work picks up where it left off once you leave the dictionary.

One word on the page becomes highlighted:

1. Use **Left/Right** to move between words in reading order, and the side **Up/Down** buttons to jump between lines. Moving the highlight repaints only the two word boxes that changed, so stepping through a line does not re-render the page.
2. Press **Confirm** to look up the highlighted word.
3. Press **Back** to return to the reader.

On the very first lookup with a dictionary (and again whenever the `.idx` or `.syn` source file changes), the reader shows *"Indexing dictionary…"* while it builds small sidecar files next to them — a `.qidx` for the word index, and a `.sidx` when a `.syn` synonym file is present. Each sidecar is rebuilt independently, only when its own source changes. This takes a few seconds for large dictionaries and makes all subsequent lookups fast. The sidecars can be deleted safely at any time — they will simply be rebuilt.

### How Lookup Works

1. **Direct match** — the word is found as-is (case-insensitive) in the dictionary index. Surrounding punctuation is ignored.
2. **Synonyms** — on a miss, if the dictionary ships a `.syn` file, alternate spellings and irregular forms recorded there are resolved to their headword (e.g. `oxen` → `ox`, `colour` → `color`). This step is skipped if the `.sidx` sidecar could not be built (e.g. transient low memory during indexing); the dictionary otherwise stays usable, and the build is retried the next time it is opened.
3. **Stemming** — still no match: common English word forms are retried automatically: possessives and plurals (`dogs` → `dog`, `stories` → `story`) and verb endings (`walked` → `walk`, `running` → `run`, `making` → `make`).
4. **Not found** — a short popup appears and you return to word selection.

## The Definition Screen

When a word is found, the definition screen shows the matched headword at the top and the definition text below, with a page counter for long definitions.

HTML dictionaries that declare `sametypesequence=h` use the EPUB text-layout engine for semantic formatting such as headings, bold, italics, lists, and line breaks. Images and CSS styling are ignored. Definitions that are too large or cannot be laid out within the available memory fall back to plain text.

- **Left/Right** or side **Up/Down** — previous / next page
- **Confirm** — look the same word up in the next installed dictionary, cycling through them. Only offered when more than one dictionary is installed; the current one is named to the right of the headword. The choice is remembered, so later lookups use it too.
- **Back** — return to word selection

Definitions render in Noto Sans at a fixed size rather than in your reading font. That is the one font carrying the phonetic alphabet — a dictionary's pronunciation notation needs about 100 codepoints that no reading font here ships, and putting them in every font and size would cost roughly five times the flash. Any character still missing falls back to the closest available shape (schwa as `e`, the primary-stress mark as an apostrophe) rather than an empty box.



## Where to find dictionaries

> credit to https://github.com/koreader/koreader/wiki/Dictionary-support for the list.

- The [reader.dict](https://www.reader-dict.com) (ex "BoboTiG/ebook-reader-dict") project provides StarDict version of daily dumps of [Wiktionary](https://www.wiktionary.org/) monolingual dictionaries for a variety of languages. It also provides [non-free multilingual](https://www.reader-dict.com) dictionaries.
- The [WikDict](https://www.wikdict.com) project provides free bilingual dictionaries based on [Wiktionary](https://www.wiktionary.org/) for a lot of language pairs. StarDict versions can be [downloaded from here](https://download.wikdict.com/dictionaries/stardict/).
- The [`Vuizur/Wiktionary-Dictionaries`](https://github.com/Vuizur/Wiktionary-Dictionaries) repository contains dictionaries based on [Wiktionary](https://www.wiktionary.org/) from many languages to English, including English-English.
- The [DictInfo](https://www.dictinfo.com/) website provides outdated monolingual dictionaries based on [Wiktionary](https://www.wiktionary.org/).
- The [Firedict site](https://tuxor1337.frama.io/firedict/dictionaries.html) contains a list of freely available dictionaries.
- [wiktionary_stardict](https://xxyzz.github.io/wiktionary_stardict/): update monthly.
- [Fictionaries](https://fictionary.gumroad.com/) provides dictionaries for various speculative fiction books and series.
- [World Factbooks Archive](https://github.com/MilkMp/CIA-World-Factbooks-Archive-1990-2025) provides 36 years of CIA's World Factbook dictionaries in StarDict format.
- [StarDict-Hebrew](https://github.com/Uri-Tauber/StarDict-Hebrew) Hebrew-English StarDict versions of Babylon dictionaries.
