# Clipping Selection, Export, and Sync

For saving your first excerpt, see [Clippings and highlights](../USER_GUIDE.md#clippings-and-highlights).

## Adjust a touch selection

Hold a word to select it, then drag either handle to adjust the range. Releasing a handle keeps the selection open. Tap outside the text to dismiss it.

To extend onto the next page, hold the ending handle at the bottom edge or over the final word for one second. A selection can cover up to three pages in the same chapter, with a limit of 240 selectable words.

Look Up searches the selected text in your dictionary. Bookmark toggles the bookmark on the selection's final page. With buttons, Back closes an open action popup without changing the selection. Once the popup is closed, Back clears the selected range. Press Back again to leave selection.

## Open a saved passage

Open View Clippings in the reader menu. Select an excerpt, then choose Open to read it or Go to Passage to return to its location.

New clippings save their position in the book's source text. Their highlights survive changes to fonts, spacing, orientation, and pagination. Go to Passage is available for these clippings.

Older clippings without a source position can still be read or deleted. Their highlights appear only when the current layout matches the saved layout.

Hold Confirm for about 0.7 seconds on an excerpt, or long-press its row, to request deletion. On touch devices, you can also hold highlighted text in the book to remove its clipping.

## Storage limits

Each book can store up to 1,024 clippings on ESP32-S3 devices or 256 on ESP32-C3 devices. The same limits apply to clippings downloaded through sync.

The clipping store in `.crosspoint/clippings` holds up to 4 KiB of UTF-8 text per clipping. Shorten a selection that exceeds this limit before saving.

## Text export and deletion

CrossPoint appends up to 2,000 UTF-8 bytes of each saved excerpt to `My Clippings.txt` in the SD card's root folder. This export uses Kindle-compatible formatting. If export fails, the clipping remains saved and CrossPoint reports the export failure.

`My Clippings.txt` is a separate text export. CrossPoint does not use it to restore or sync highlights. Deleting a clipping does not remove its historical text export.

Local book and folder moves carry clipping data with them. Deleting a local book removes its local clipping data. It leaves synced server copies and entries in `My Clippings.txt` unchanged.

Deleting an individual clipping queues its server deletion for the next manual sync with Sync Clippings enabled. See [progress sync](progress-sync.md#syncing-while-reading) for server compatibility and sync behavior.
