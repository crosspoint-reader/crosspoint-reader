# Writer Viewport And Cursor Design

## Summary

The next Writer stage should move from a tail-following draft pad to a document-based editor view. Writer will keep a full in-memory document buffer for now, track a cursor as an insertion point within that document, and maintain viewport state so the cursor can move through the whole draft while remaining visible on screen.

This stage focuses on navigation and rendering behavior, not full editing semantics. The architecture should support later insert-at-cursor editing without needing to replace the navigation model.

## Goals

- Support movement through the full draft rather than only following the tail.
- Introduce a visible caret that marks the insertion point between characters.
- Keep the cursor visible by scrolling the viewport when needed.
- Reuse the existing input model boundaries:
  - text entry through `WriterInput`
  - device-style navigation through the existing button input path
- Build toward insert-at-cursor editing without implementing every editing feature in the same step.

## Non-Goals

- No overwrite mode.
- No word-wise movement yet.
- No multi-draft workflow in this stage.
- No large-document caching or paging subsystem yet.
- No simulator-only arrow-key path separate from the existing button framework.

## Input Design

Writer should keep the current split between text entry and navigation input.

- Simulator text input remains on the `WriterSimInput` path.
- Arrow keys in the simulator continue to flow through `HalGPIO` as device-style button input.
- Real hardware buttons should use the same Writer movement functions as simulator arrows.

This keeps the simulator seam narrow and avoids building separate movement behavior for host keyboard navigation versus device navigation.

## Cursor Model

Writer should represent the cursor as an insertion index into the document text.

- The caret sits between characters.
- Moving left or right changes the insertion index by one character.
- Future typing should insert text at that index.
- For this stage, movement support may arrive before full insert-at-cursor editing, but the in-memory model must already assume insertion can happen anywhere.

The cursor must always remain visible on screen.

## Movement Model

### End Goal

- Left / Right short press: move one character.
- Left / Right long press: move to start or end of the current line.
- Up / Down short press: move one visual line.
- Up / Down long press: page up or page down.

Keyboard equivalents for the end goal:

- `Left` / `Right`: one character
- `Up` / `Down`: one visual line
- `Home` / `End`: line start / line end
- `Page Up` / `Page Down`: page jump

### Increment Strategy

The first implementation increment should prioritize:

- visible caret
- left / right movement
- up / down movement
- viewport scrolling to keep the caret visible

Long-press navigation and additional keyboard keys can be added after the first cursor-aware viewport pass is working.

## Viewport Model

Writer should stop rendering from "the tail of all wrapped text" and instead render from viewport state tied to the current cursor position.

The viewport should be defined in terms of visible wrapped lines, not just raw bytes or raw newline-delimited paragraphs. The implementation may derive those wrapped lines from the in-memory document for now.

When a cursor move would place the caret off-screen:

- scroll the viewport by approximately half a page
- keep the caret visible after the scroll
- prefer preserving context over snapping to a completely new page

Half-page scrolling is the preferred default because it balances context retention with e-ink friendliness better than single-line scrolling or full-page jumps.

## Rendering

Writer should render:

- the visible wrapped lines for the current viewport
- a caret between characters at the current insertion point
- the existing footer with battery, filename, and word count

The caret should be a simple insertion caret, not a block selection or overwrite indicator.

## Storage And Flush Behavior

The existing storage flow remains acceptable for this stage:

- Writer still reads the draft from disk on entry
- Writer still flushes buffered changes to disk through `WriterDraftStore`
- Back and Confirm keep their current flush behavior unless later editing changes require refinement

This stage is primarily about document movement and viewport state, not about changing the disk persistence model.

## Memory Strategy

This stage deliberately keeps a simple in-memory document model.

Assumption:

- current draft sizes used during development are acceptable to hold in memory

Deferred concern:

- if larger documents create memory pressure, Writer should later borrow windowing/caching ideas from the TXT reader rather than introducing premature complexity now

This means the current design optimizes for learning and correctness first, while leaving room for a future cache-backed document model.

## Components

Expected component responsibilities after this stage begins:

- `WriterActivity`
  - owns editor state
  - handles movement commands
  - coordinates rendering and flush behavior
- `WriterInput`
  - provides text input events
- button input path (`MappedInputManager` / existing activity input flow)
  - provides movement commands
- `WriterDraftStore`
  - continues to own draft file read/write behavior
- `WriterVisibleLines`
  - may evolve or be replaced if viewport rendering needs a more cursor-aware helper

The existing TXT reader is a useful reference for:

- viewport sizing
- UTF-8-safe wrapping
- page/progress bookkeeping

However, it is not a drop-in editor layout engine because it tracks reader pagination and file offsets rather than caret positions within wrapped lines. Writer will likely need its own cursor-aware wrapped-line layout helper, even if that helper borrows wrapping rules from `TxtReaderActivity`.

## Error Handling

- Existing save-failure behavior should remain in place.
- Navigation errors should fail safely by clamping movement at document boundaries.
- Cursor movement must never leave the cursor outside the valid document range.

## Testing Strategy

Manual simulator checks for this stage should include:

- moving left and right across a line
- moving across line boundaries
- moving up and down through wrapped lines
- viewport scrolling when the caret reaches the top or bottom edge
- movement at document start and end without crashes or invalid positions
- footer still rendering correctly during movement

Targeted helper tests are preferred where movement or viewport math can be extracted into small pure functions.

## Open Follow-Ups

- insert-at-cursor editing
- long-press navigation behavior
- `Home` / `End` / `Page Up` / `Page Down` keyboard support in the simulator
- keyboard activity contributing to sleep timer resets
- large-document caching/windowing
- multi-draft support and user-controlled draft naming
