# X4 WriterDeck MVP Design

## Context

This project turns the Xteink X4 into a distraction-free writerdeck while keeping its reader firmware useful. The base firmware should be upstream CrossPoint Reader, not MicroSlate and not a fork-of-a-fork. CrossInk is a reference for simulator support, future font work, and feature-integration patterns. MicroSlate is only a reference for implementation ideas if we get stuck.

CrossInk's reading-stats work is a useful example of adding a feature without disturbing the core reader path. It uses small versioned stats records and a dedicated stats activity, which is a good template for future writer-specific features such as word count, session time, or daily writing totals.

The first goal is not a complete writing product. The first goal is a trusted development base, then the smallest local writing loop that protects text.

## Goals

Stage 0 proves the base:

- fork upstream `crosspoint-reader/crosspoint-reader`
- build unmodified CrossPoint
- flash only after the user chooses to test on hardware
- add or port simulator support before writer features
- keep the upstream reader behavior intact

Stage 1 adds a fork-local writer mode:

- a `Write` activity alongside CrossPoint reader functionality
- one active plain-text draft
- simulator-driven keyboard input, with hardware/Bluetooth input deferred
- basic line-oriented editing
- e-ink rendering of the live writing surface
- manual save, Enter commit, idle checkpoint, and save before sleep
- recovery of active draft plus unfinished input after restart

## Non-Goals

- No OLED support.
- No LED support.
- No custom Wi-Fi implementation. CrossPoint already owns Wi-Fi, file transfer, OTA, and related device behavior.
- No font changes in Stage 1. Font work is active upstream and should be revisited after CrossPoint SD/custom-font work settles.
- No plugin/runtime scripting dependency. CrossPoint plugin-style extensibility is still exploratory upstream.
- No full-document editor in Stage 1.
- No Markdown rendering in Stage 1.
- No web-based draft editor in Stage 1.

## Upstream Strategy

Fork at the start of Stage 0.

Repository conventions:

- `upstream` points to `crosspoint-reader/crosspoint-reader`.
- `origin` points to the user fork.
- the upstream-tracking branch stays clean and tracks `upstream/master`.
- writerdeck work happens on named topic branches.

Suggested branches:

- `stage-0-simulator`
- `stage-1-writer-activity`
- `stage-1-editor-core`
- `stage-1-storage-recovery`

Useful tracking commands:

```bash
git fetch upstream
git log --oneline upstream/master..HEAD
git diff upstream/master...HEAD
git range-diff upstream/master old-branch new-branch
```

Early work should prefer rebasing topic branches on `upstream/master`. If writerdeck changes become broad enough that rebases are too costly, switch to a documented integration branch that merges upstream regularly.

## Architecture

The writer mode should follow CrossPoint activity patterns rather than creating a separate app runtime. It should be an activity reachable from the existing UI, using CrossPoint's display, input, SD card, power, and settings foundations.

The editor/session module is the source of truth. Display code consumes session state; it never owns the document.

CrossPoint and CrossInk already use mapped device buttons for menu navigation. Future writer navigation should reuse that button/input layer rather than introducing a separate physical-button model. CrossInk's `ButtonNavigator` is a concrete reference for wraparound list navigation and hold-to-repeat behavior when draft browsing and richer editor navigation are added.

Stage 1 keeps the older writerdeck design's storage-first model:

- active draft filename
- current editable line buffer
- cursor within the current line
- dirty/checkpoint state
- last save reason
- restored-on-boot flag

The committed draft file is plain text on SD. The unfinished current line is stored in a small recovery/checkpoint record. This avoids per-key writes while protecting recent input.

## Editor Behavior

Stage 1 uses a line-oriented append model.

Supported keys:

- printable characters insert at cursor
- `Enter` commits the current line and appends a newline
- `Backspace` deletes before cursor
- `Left` and `Right` move within the current line
- `Ctrl/Cmd+S` saves current line without appending newline

Deferred keys:

- `Home`
- `End`
- `Delete`
- Up/down full-document navigation
- selection
- copy/paste
- undo

The line-oriented model is intentionally conservative. The X4 e-ink display is fast enough for live rendering, but storage safety and testability matter more in Stage 1 than a full editor.

## Persistence

The save model has four triggers:

- manual save
- Enter commit
- idle checkpoint
- save before sleep or restart path when available

The active draft is a `.txt` file. The recovery record stores:

- active draft name
- current line bytes
- line length
- cursor
- dirty flag
- save/checkpoint reason

On boot:

- if recovery points to an existing draft, restore it
- if recovery is malformed, clear it
- if no active draft exists, select the first draft or create `draft.txt`

## Display

The X4 e-ink screen is the live writing display in Stage 1.

The renderer should show:

- active draft name
- committed text context
- current line with cursor
- lightweight save/dirty/restored status

Refresh policy should be simple at first:

- refresh after accepted key events
- refresh after save/checkpoint completion when visible state changes
- refresh after restore

If refresh behavior becomes distracting or power-heavy, add a measured refresh policy in Stage 2.

## Testing

Development is test-driven.

Stage 0 verification:

- unmodified CrossPoint builds
- simulator target builds
- simulator opens and displays a known screen
- user handles hardware flashing

Stage 1 host tests:

- editor line insert/delete/cursor behavior
- keyboard report decoding and shortcut mapping
- save policy decisions
- recovery blob encode/decode
- boot policy for restored/missing/no draft

Stage 1 simulator tests:

- Write activity can open
- typed keys update session state
- e-ink render reflects current line and cursor
- save/checkpoint status is visible

Hardware tests are user-led:

- flash firmware
- pair/connect Bluetooth keyboard
- create/open draft
- type text
- verify SD `.txt` content
- reboot and verify unfinished input recovery

## Risks

Memory remains the central risk. CrossPoint has active discussions around heap fragmentation, custom fonts, dictionary work, and large documents. Writer mode should avoid dynamic allocation where practical and keep buffers explicitly bounded.

Upstream scope is another risk. CrossPoint currently treats notepads and complex typed notes as out of scope, so writer mode should remain fork-local unless upstream direction changes.

Simulator support is high priority because it reduces hardware iteration cost. If simulator support becomes too invasive, keep it on a separate branch and do not block plain firmware builds.

## Later Stages

Likely Stage 2 work:

- Home/End/Delete
- draft browser integration
- device-button draft browsing and navigation using CrossPoint's existing mapped button model
- better render cadence
- safe SD write rotation if CrossPoint storage does not already provide enough protection
- import/export workflow through existing CrossPoint file transfer
- font work after upstream font direction settles
- writer stats using CrossInk's reading-stats pattern as a reference

Likely Stage 3 work:

- full-document cursor movement
- Markdown-aware writing or preview
- richer file management
- optional sync/export flows
