# Dictionary

## Supported Format

The reader supports **StarDict** dictionaries. When searching for dictionaries online, look for "StarDict format" or files with `.dict`, `.idx`, and `.ifo` extensions.

A dictionary folder typically contains:

- `.dict` or `.dict.dz` -- definition data (`.dz` is compressed and will be decompressed automatically on first use)
- `.idx` -- word index (required)
- `.ifo` -- metadata such as dictionary name and word count (recommended)
- `.syn` -- alternate forms and synonyms (optional, enhances lookup coverage)

Minimum requirement: `.dict` (or `.dict.dz`) and `.idx`. Without `.ifo`, the dictionary will still work but metadata and HTML definition rendering may be limited.

---

## Setting Up a Dictionary

1. Copy your dictionary folder(s) to one of these directories on the SD card:
   - `/.dictionaries/` (checked first)
   - `/dictionaries/`
2. Open **Settings -> Dictionary** on the device.
3. Select a dictionary from the list.

To view a dictionary's metadata before selecting it, long-press the entry. The info screen shows the dictionary name, word count, alternate form count, and type. Press **Confirm** to see the raw info file, or **Back** to return to the list.

To deselect the current dictionary, select **None** from the list.

### Preparation

Some dictionaries need to be prepared before first use -- typically those downloaded in compressed form. When you select such a dictionary, a confirmation screen lists the required steps (decompression, index generation). Press **Confirm** to proceed or **Back** to cancel.

Preparation runs on-device and can take a few seconds to several minutes depending on dictionary size. Progress is shown step by step. If preparation fails or is cancelled, the previous dictionary selection is kept.

Once prepared, the dictionary is ready for all future use without repeating this step.

---

## Looking Up a Word

The **Look Up Word** option in the reader menu is only visible when a dictionary is active.

1. Open the reader menu and choose **Look Up Word**.
2. The page becomes a word-select overlay -- one word is highlighted, initially near the centre of the page.
3. Use **Up/Down** to move between rows, **Left/Right** to move between words on the same row.
4. Press **Confirm** to look up the highlighted word.
5. Press **Back** to exit word-select without looking anything up.

### How Lookup Works

When you select a word, the reader searches for it in this order:

1. **Direct match** -- the word is found as-is in the dictionary index.
2. **Stemming** -- the reader automatically tries common word forms (plurals, verb conjugations, comparatives). For example, "running" finds "run".
3. **Alternate forms** -- if the dictionary includes a synonym/alternate forms file and no match was found yet, a prompt appears. Press **Confirm** to search alternate forms, or **Back** to skip.
4. **Suggestions** -- if nothing matched, a list of similar words from the dictionary is shown. Select one to view its definition.
5. **Not found** -- if no matches or suggestions exist, a not-found message appears. Press **Back** to return to word-select, or **Confirm** to exit to the reader.

---

## The Definition Screen

When a word is found, the definition screen shows the headword at the top and the definition text below.

- **PageForward / Right** -- next page (for long definitions)
- **PageBack / Left** -- previous page
- **Confirm** (labelled **Look Up Word**) -- enter word-select mode on the definition text (see Chaining Lookups below)
- **Back** (short press) -- return to the previous screen
- **Back** (long press) -- exit all the way back to the reader

### Definition Font Settings

The definition viewer has its own font settings, independent of the reader font:

- **Definition Font Family** -- choose a different font family for definitions, or leave as Global to match the reader font.
- **Definition Font Size** -- choose a different size for definitions, or leave as Global to match the reader font.

Both settings are in **Settings**.

---

## Chaining Lookups

From a definition screen, you can look up any word within the definition text without returning to the reader.

1. Press **Confirm** (**Look Up Word**) on the definition screen.
2. A word in the definition becomes highlighted. Navigate to any word and press **Confirm**.
3. A new definition screen opens for that word.
4. You can chain further -- press **Look Up Word** again from the new definition.
5. Short-press **Back** to exit word-select and return to the current definition.
6. Short-press **Back** again to go back through the chain (each press returns to the previous definition).
7. Long-press **Back** at any point to exit directly to the reader.

---

## Phrase / Multi-word Lookup

In word-select mode, you can select a sequence of words to look up as a phrase.

1. Navigate to the first word of the phrase.
2. Long-press **Confirm** to anchor on that word.
3. Use the navigation buttons to extend (or shrink) the selection to cover the full phrase. All selected words are highlighted.
4. Short-press **Confirm** to look up the selected phrase.
5. Press **Back** to cancel and return to single-word select mode.

Multi-word select works in both the reader word-select and the definition word-select.

**Limitation:** Multi-word selection cannot span a page boundary. If a phrase crosses from one page to the next, only the words on the current page are available for selection. As a workaround, reduce the reader font size so more words fit on a single page, perform the lookup, then restore the original font size.

---

## Per-Book Dictionary

Each book can have its own dictionary, independent of the global setting.

1. Open the reader menu and choose **Book Dictionary**.
2. Select a dictionary from the picker. If the dictionary needs preparation, the same preparation flow runs as described in Setting Up.
3. The per-book choice is saved and restored each time the book is opened.
4. To remove the override, open **Book Dictionary** and select **Use Global**. The picker shows the current global dictionary name in parentheses next to Use Global.

The global dictionary is automatically restored whenever the book is closed. Changing a book's dictionary does not affect the global setting or other books.

---

## Lookup History

Each book maintains its own lookup history, accessible from the reader menu.

1. Open the reader menu and choose **Lookup History**.
2. Each entry shows the searched word and a status indicator:
   - Checkmark -- found directly
   - Tilde (~) -- resolved via stemming
   - Question mark (?) -- reached via the suggestions screen
   - X -- not found
3. Select any entry and press **Confirm** to look it up again. Press **Back** from the definition to return to the history screen.
4. To delete an entry, long-press **Confirm** on it. A confirmation popup appears -- press **Confirm** to delete, or **Back** to cancel.

The history list is capped at the **Dictionary History Limit** value in Settings. When the cap is reached, the oldest entry is removed to make room for the new one.

---

## IPA Phonetic Characters

Dictionary definitions containing IPA phonetic transcriptions (pronunciation symbols like /ˈæp.əl/) are rendered using a dedicated built-in font that covers all standard IPA characters. IPA symbols appear automatically in this font regardless of your chosen reading font or size.

If you see a filled diamond where a pronunciation symbol should appear, that character is outside the supported IPA range.

---

## Offline Dictionary Tools

A command-line tool is included for working with StarDict dictionaries on your computer, without the device. It requires Python 3 and has no external dependencies.

### Pre-processing

Replicate the on-device preparation steps (decompression and index generation) on your computer:

```bash
python3 scripts/dictionary_tools.py prep /path/to/dictionary-folder
```

This is useful for pre-processing large dictionaries before copying them to the SD card, avoiding the longer on-device preparation time.

### Looking Up a Word

Look up a word from the command line:

```bash
python3 scripts/dictionary_tools.py lookup /path/to/dictionary-folder apple
```

Prints the definition to stdout. The dictionary must be prepared first (either on-device or via `prep`).

### Merging Dictionaries

Combine two or more StarDict dictionaries into a single monolithic dictionary:

```bash
python3 scripts/dictionary_tools.py merge \
  --source /path/to/dict-a \
  --source /path/to/dict-b \
  --output /path/to/merged-dict
```

Specify `--source` once per dictionary to include. The merged output contains the full union of all headwords and synonyms. When the same word appears in multiple sources, definitions are concatenated in source order.

Source dictionaries must be prepared (decompressed `.dict` files) before merging. The output is a complete, ready-to-use StarDict dictionary that can be copied directly to the SD card.
