# PR title (suggestion)

`feat(reader): EPUB clippings (multi-page capture) stacked on bookmarks`

Paste everything below the line into the GitHub PR description.

---

## Why this targets PR #1337 ([feat: epub bookmarks](https://github.com/crosspoint-reader/crosspoint-reader/pull/1337))

This branch is built **on top of the bookmarks work** (`feature/bookmarks` / PR #1337), not on `master` alone.

- **Review stays focused:** Reviewers see only the **clippings + capture** delta, not the whole bookmarks feature again.
- **Correct merge order:** Bookmarks land first; this PR then merges into the same line of development (or into `master` after #1337 merges, with a quick rebase if needed).
- **Shared pieces:** Clippings reuse the same patterns as bookmarks (list UI, `ButtonNavigator`, `ClippingStore` parallel to bookmark storage, `Page` / `TextBlock` text extraction used for capture).

**When opening the PR on GitHub:** set **base** to the branch that #1337 uses (e.g. compare across forks: base repo/branch = the PR #1337 head — often `vedi0boy/crosspoint-reader` → `feature/bookmarks`, or your fork’s `feature/bookmarks` if kept in sync). If #1337 is already merged into `master`, switch the PR base to `master` instead.

## Summary

- **Capture** passages across multiple pages while reading an EPUB; **save** to SD under `.crosspoint/clippings/`.
- **Reader menu:** **Capture**, **Clippings** (list + viewer + delete), integrated with bookmark-style reader flows.
- **Clippings list:** Book-style ordering (sort by book %, spine, page), previews, subtitles, bookmark icon in portrait (same as bookmarks list).
- **Status bar / popups:** capture progress while capturing; save/fail uses `drawPopup` like bookmarks; **menu while capturing discards** capture; **page back** finishes capture **without** turning the page.

## How it works (quick reference)

| Action | While capturing |
|--------|-----------------|
| Page **forward** | Turn page **and** append that page to the capture |
| Page **back** | **Save** clipping, show popup, **stay on current page** |
| **Confirm** (menu) | **Cancel** capture, open reader menu |
| **Short Back** | Cancel capture (discard) |

Storage: `ClippingStore` writes index + markdown under `.crosspoint/clippings/<hash>.{idx,md}`.

## Documentation

- User-facing: **`USER_GUIDE.md`** → section **§7 Clippings (captures)**.

## Screenshots (optional)

- **On device:** Power + Volume Down (see [Taking a Screenshot](USER_GUIDE.md#taking-a-screenshot)) or reader menu → **Take screenshot** → files on SD under `screenshots/`.
- **Dev:** `python3 scripts/debugging_monitor.py` can capture `SCREENSHOT_START` / `SCREENSHOT_END` over serial to a `.bmp` (see script header).
- **PR:** Drag images into the GitHub description or paste from clipboard; same as #1337’s embedded images.

## Testing

- [ ] `pio run` / `pio check`
- [ ] Capture: multi-page, save, cancel (Back / menu), list order, delete
- [ ] Bookmarks from #1337 still work (hold confirm, list, delete)
