# Dictionary

## Setting Up a Dictionary

Place your dictionary folder(s) in the `/dictionary/` directory on the SD card, then follow these steps.

1. Open **Settings → Dictionary**.
2. Select a dictionary from the list.
3. Long-press any entry to view its metadata (name, word count, type). From the metadata view, press the select button to see the raw dictionary info file, and the back button to return to the list.

**Preparation.** The first time you select a dictionary it may need to be prepared — the reader decompresses and indexes it on-device. A confirmation screen lists the steps involved; press the select button to proceed or the back button to cancel without changing your current dictionary. Preparation can take a few seconds to several minutes depending on dictionary size. Progress is shown step by step as each stage completes. If preparation fails, the previous dictionary selection is kept.

---

## Looking Up a Word

The **Look Up Word** option in the reader menu is only visible when a dictionary is active.

1. Open the reader menu and choose **Look Up Word**.
2. Use the navigation buttons to move the highlight to the word you want.
3. Press the select button to look it up.

The reader tries a direct match first, then automatically tries stemming (handles plurals, verb conjugations, and comparative forms). If the dictionary has alternate forms and no match is found, a prompt appears — press the select button to check alternate forms, or the back button to skip. If still no match is found, a suggestions screen appears with nearby words; select one to open its definition. If nothing matches at all, a not-found message is shown.

---

## The Definition Screen

1. Use the page navigation buttons to move between pages on long definitions.
2. Press the select button (**Look Up Word**) to enter word-select mode on the definition text — see [Chaining Lookups](#chaining-lookups).
3. Long-press the back button at any time to return directly to the reader.

---

## Chaining Lookups

From a definition screen, you can look up any word within the definition text without returning to the reader.

1. Press the select button (**Look Up Word**) on the definition screen.
2. Navigate to any word in the definition text and press the select button to look it up.
3. A new definition screen opens. You can press **Look Up Word** again to chain further.
4. Short-press the back button to exit word-select mode and return to the current definition.
5. Long-press the back button to exit all the way back to the reader.

---

## Phrase / Multi-word Lookup

In word-select mode, you can select a sequence of words to look up as a phrase.

1. In word-select mode, navigate to the first word of the phrase.
2. Long-press the select button to anchor on that word.
3. Use the navigation buttons to extend (or shrink) the selection to cover the full phrase.
4. Short-press the select button to look up the selected phrase.
5. Press the back button to cancel and return to single-word select mode.

---

## Per-Book Dictionary

Each book can have its own dictionary, independent of the global setting in Settings.

1. Open the reader menu and choose **Book Dictionary**.
2. Select a dictionary from the picker. The picker closes and that dictionary is used for this book.
3. The per-book choice is saved and restored each time the book is opened.
4. To remove the override, open **Book Dictionary** and select **Use Global**. The global dictionary is also automatically restored whenever the book is closed.

---

## Lookup History

Lookup history is stored per-book and accessible from the reader menu.

1. Open the reader menu and choose **Lookup History**.
2. Each entry shows the searched word and a status glyph:
   - `√` — found directly
   - `~` — resolved via stemming
   - `?` — reached via the suggestions screen
   - `×` — not found
3. Select any entry and press the select button to re-look it up. Pressing the back button from the resulting definition returns to the history screen.
4. Long-press the select button on any entry to enter delete mode. Press the select button to confirm deletion, or the back button to cancel.

The history list is capped at the **History Limit** value in Settings. When the cap is reached, the oldest entry is removed to make room for the new one.
