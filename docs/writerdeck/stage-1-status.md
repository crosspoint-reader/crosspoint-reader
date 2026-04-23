# Writer Stage 1 Status

This note captures the current state of the Writer activity on the X4 fork.

## What Works

- Writer is reachable from the Home menu as its own activity.
- The draft file is created automatically at `/writer/draft.txt`.
- Simulator input is accepted through terminal stdin and displayed in Writer.
- Confirm flushes the current Writer buffer to disk.
- Back also flushes the current Writer buffer before exiting.
- Long wrapped paragraphs keep the newest visible lines on screen.
- Large drafts are displayed from the tail so Writer stays usable without loading the entire file into memory.
- The footer shows battery status, draft filename, and word count.

## Current Limits

- Simulator input is still line-buffered by the host terminal, so typed text does not reach Writer until Enter is pressed in the terminal.
- Large drafts are displayed from the last 64KB only; there is no paging, scrolling, or cursor-aware viewport yet.
- Tail loading may begin mid-line or mid-character for very large files.
- Keyboard input does not yet count as device activity for sleep timing.

## Likely Next Steps

- Replace simulator stdin polling with a more realistic per-key input path.
- Count real keyboard events as activity so sleep policy matches writing behavior.
- Introduce a document viewport model for scrolling, cursor movement, and larger drafts.
- Revisit draft loading so viewport state is decoupled from file storage.
