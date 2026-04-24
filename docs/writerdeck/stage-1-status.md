# Writer Stage 1 Status

This note captures the current state of the Writer activity on the X4 fork.

## What Works

- Writer is reachable from the Home menu as its own activity.
- The draft file is created automatically at `/writer/draft.txt`.
- Simulator input is accepted per key through the simulator input bridge and displayed in Writer.
- Confirm flushes the current Writer buffer to disk.
- Back also flushes the current Writer buffer before exiting.
- Writer has a visible caret and supports left, right, up, and down movement through the current draft window.
- The viewport scrolls to keep the caret visible while navigating wrapped lines.
- Large drafts are displayed from the last 64KB so Writer stays usable without loading the entire file into memory.
- The footer shows battery status, draft filename, and word count.

## Current Limits

- Text entry still appends to the end of the draft rather than inserting at the caret.
- Large drafts are displayed from the last 64KB only; there is no full-document paging or cache-backed editor window yet.
- Tail loading may begin mid-line for very large files.
- Wrapped lines are currently estimated by character count rather than measured rendered width.
- Keyboard input does not yet count as device activity for sleep timing.

## Likely Next Steps

- Improve Writer wrapping so line breaks reflect the rendered pixel width.
- Replace append-only text entry with insert-at-caret editing.
- Count real keyboard events as activity so sleep policy matches writing behavior.
- Revisit draft loading so viewport state is decoupled from file storage.
