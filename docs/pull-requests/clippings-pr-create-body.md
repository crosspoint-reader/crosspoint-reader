## Why this relates to PR #1337 ([feat: epub bookmarks](https://github.com/crosspoint-reader/crosspoint-reader/pull/1337))

This branch extends the **bookmarks** reader work: same list patterns, `Page`/`TextBlock` text extraction, and menu integration. It is easiest to review **after** #1337 is familiar.

**Clippings-only diff vs the bookmarks branch** (for reviewers who want to skip re-reading bookmark commits):

`https://github.com/crosspoint-reader/crosspoint-reader/compare/vedi0boy:feature/bookmarks...tnaftali:feature/clippings-on-bookmarks`

(Compare across forks: bookmarks head → this branch.)

## Summary

- **Multi-page capture** while reading: save passages to `.crosspoint/clippings/` on the SD card.
- **Reader menu:** **Capture**, **Clippings** (list, full-text viewer, delete).
- **UX:** Status bar shows capture progress; **page back** finishes capture and saves **without** turning the page; **Confirm (menu)** or **short Back** **cancel** an in-progress capture; save/fail uses a centered popup like bookmarks.
- **List:** Ordered by position in the book; previews and metadata in subtitles.

## Screenshots

![Reader menu](https://raw.githubusercontent.com/tnaftali/crosspoint-reader/feature/clippings-on-bookmarks/docs/clippings-pr-screenshots/01-menu.bmp)

![Clippings list](https://raw.githubusercontent.com/tnaftali/crosspoint-reader/feature/clippings-on-bookmarks/docs/clippings-pr-screenshots/02-clippings.bmp)

![Capturing (status bar)](https://raw.githubusercontent.com/tnaftali/crosspoint-reader/feature/clippings-on-bookmarks/docs/clippings-pr-screenshots/03-capturing.bmp)

![Clipping saved (popup)](https://raw.githubusercontent.com/tnaftali/crosspoint-reader/feature/clippings-on-bookmarks/docs/clippings-pr-screenshots/04-clipping-saved.bmp)

![Clipping detail (viewer)](https://raw.githubusercontent.com/tnaftali/crosspoint-reader/feature/clippings-on-bookmarks/docs/clippings-pr-screenshots/05-clipping-detail.bmp)

![Delete confirmation](https://raw.githubusercontent.com/tnaftali/crosspoint-reader/feature/clippings-on-bookmarks/docs/clippings-pr-screenshots/06-clipping-delete-confirmation.bmp)

## Documentation

- `USER_GUIDE.md` — **§7 Clippings (captures)**

## Testing

- [x] `pio run` (local)
- Capture / cancel / save / list / delete on device
