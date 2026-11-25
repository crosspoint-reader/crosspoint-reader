# Readwise Reader sync

This fork can mirror your [Readwise Reader](https://read.readwise.io) queue onto
the device as one EPUB per article, with the full article text, and report your
reading back to Readwise.

## Setup

1. Get your access token from <https://readwise.io/access_token>.
2. Put it on the device, in one of three ways:
   - **Web UI:** connect to the device's File Transfer page, open **Settings**,
     and paste the token into **Readwise → Access token**.
   - **SD card:** create `/.crosspoint/readwise.json` containing
     `{"token": "YOUR_TOKEN"}`. On the next boot it is re-saved in obfuscated
     form and the plain copy is removed.
   - **On the device:** **Settings → Connectivity → Readwise → Access token**
     (on-screen keyboard).
3. **Settings → Connectivity → Readwise → Sync now.**

Articles land in `/Readwise` (configurable). Open them from **Browse Files** or
the **Library** like any other book.

## What a sync does

1. Checks the token.
2. Lists the locations you chose under **Sync from** (default: *Later* and
   *Shortlist*; *Inbox* is optional). Articles, emails, RSS items and PDFs are
   kept; videos, tweets, highlights and notes are ignored.
3. For articles already on the device:
   - sends your reading position to Readwise (`reading_progress`), and
   - with **Archive finished articles** on (default), archives an article in
     Readwise once you reach its last page, and removes the local copy.
4. With **Remove archived articles** on (default), deletes local copies of
   articles that left the queue (archived or deleted on another device).
5. Downloads new articles, newest first, up to 50 kept on the device.

Files are tracked in `/.crosspoint/readwise-index.json`. The sync only ever
touches files it created. If you delete an article on the device, it is not
downloaded again while it stays in your queue.

## Article conversion

Readwise's cleaned `html_content` is streamed straight to the SD card through a
sanitizer that keeps the text structure (headings, paragraphs, lists, quotes,
tables, emphasis, code) and drops images, media, scripts and all attributes.
The output is well-formed XHTML by construction, so it always opens in the
reader. The article language is guessed from its text (en, nl, de, fr, es, it,
pt) for correct hyphenation; everything else is tagged English.

Articles with no readable text (for example a saved video) are skipped.

## Limits

- Readwise allows 20 list requests a minute, so each download takes about
  3 seconds. Rate-limit responses are honoured (`Retry-After`) and retried.
- Images are not downloaded.
- Like the rest of CrossPoint's networking, TLS is encrypted but certificates
  are not verified (no CA bundle ships with the firmware). Treat the token
  like a password: you can revoke it at readwise.io/access_token at any time.

## Code

- `lib/Readwise/`: host-testable pieces: `XhtmlSanitizer`,
  `JsonStringStreamer` (streams one JSON string value of any size),
  `EpubBuilder` (stored-zip EPUB 3 writer, CRC-32, language guess).
- `src/readwise/`: `ReadwiseStore` (settings) and `ReadwiseSync` (the sync
  state machine, one network request per `step()`).
- `src/activities/readwise/`: the settings and sync screens.
- Tests: `test/readwise/` and `ReadwiseArticleLaysOut` in
  `test/chapter_html_slim_parser/`. Set `READWISE_SAMPLE_DIR` to a folder of
  `.html` files to lay real articles out through the reader's parser.
- Build with `-DREADWISE_API_BASE=\"http://127.0.0.1:8765/api\"` to point the
  sync at a local mock server (used for simulator testing).
