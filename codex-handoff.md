# Codex Handoff: X4 WriterDeck

## Project Goal And Current Status

Build a distraction-free writerdeck firmware for the XTeInk-X4 eReader, starting from the trusted CrossPoint Reader firmware rather than MicroSlate or a fork-of-a-fork. The device is ESP32-based with a fast e-ink screen. Reader features should remain intact while Writer mode is added incrementally.

Current status:

- Repository: `/Users/mark/Coding/esp32/writerdeck-x4`
- Current branch: `stage-2-writer-wrap`
- Branch is clean and pushed to `origin/stage-2-writer-wrap`.
- `master` tracks `origin/master`, not upstream.
- `upstream` is `crosspoint-reader/crosspoint-reader`.
- Stage 0 and Stage 1 have been merged into `origin/master`.
- Stage 2 branch currently contains:
  - updated Stage 1 status docs
  - pixel wrapping design/spec
  - Stage 2A implementation plan
  - measured-width Writer wrapping
  - wrapped-layout cache for cursor movement

The Writer activity is usable in the simulator: it opens from Home, creates/reads `/writer/draft.txt`, accepts simulator per-key input, shows a caret, supports left/right/up/down movement, flushes on Confirm/Back, renders a footer, and now wraps by measured pixel width.

## Latest Manual Verification

On `stage-2-writer-wrap`, measured pixel wrapping was manually checked in the simulator with normal prose, a long word, many narrow `i` characters, and many wide `W` characters. Wrapping behavior looked correct. Cursor movement initially slowed after measured wrapping, then became much more responsive after adding the `WriterActivity` wrapped-layout cache. First render/open of larger drafts may still have a slight delay.

## Tech Stack And Important Commands

Tech stack:

- C++20
- PlatformIO
- ESP32 / Arduino framework
- CrossPoint Reader activity architecture
- X4 simulator dependency from `mahood73/crosspoint-simulator#dev`
- SDL2 simulator on Apple Silicon/macOS
- Host helper tests via shell scripts

Important commands:

```bash
# Branch/status
git status --short --branch
git branch -vv
git fetch --all --prune
git rev-list --left-right --count master...upstream/master

# Writer helper tests
bash test/run_writer_cursor_test.sh
bash test/run_writer_wrapped_layout_test.sh
bash test/run_writer_viewport_test.sh
bash test/run_writer_visible_lines_test.sh

# Builds
pio run -e simulator
pio run

# Run simulator
pio run -e simulator -t run_simulator

# Stage 2 branch push
git push -u origin stage-2-writer-wrap
```

Useful local simulator files:

- `fs_/writer/draft.txt`
- `fs_/writer/big_draft.txt`
- `fs_/writer/alice.txt`

`fs_/` is simulator SD-card state and should remain ignored.

## Repository Structure

Most relevant project areas:

- `src/activities/`
  - CrossPoint activity system.
  - `ActivityManager` owns navigation between activities.
- `src/activities/home/`
  - Home menu integration for Writer.
- `src/activities/writer/`
  - Writer activity and helper modules.
- `lib/GfxRenderer/`
  - Rendering primitives, including `GfxRenderer::wrappedText()` reference behavior.
- `lib/Utf8/`
  - UTF-8 helpers reused for cursor and wrapping safety.
- `test/writer_*`
  - Standalone host tests for Writer helpers.
- `docs/superpowers/specs/`
  - Design docs/specs.
- `docs/superpowers/plans/`
  - Implementation plans.
- `docs/writerdeck/`
  - WriterDeck workflow/status notes.

## Key Architectural Decisions

- Base firmware is upstream CrossPoint Reader.
- CrossInk is only a reference for simulator/font/feature patterns.
- MicroSlate is only a reference if stuck.
- Writer is a CrossPoint `Activity`, not a separate app runtime.
- `ActivityManager::goToWriter()` navigates to Writer.
- Home menu has a translated `STR_WRITER` label.
- Writer storage is local and simple:
  - draft path: `/writer/draft.txt`
  - draft folder: `/writer`
  - append-only writes for now
  - reads last 64KB tail window for large files
- Writer text model currently uses:
  - `draftText`: loaded/committed draft text
  - `inputBuffer`: unflushed input
  - `cursorIndex`: byte offset insertion point in rendered text
- Text entry still appends to end; insert-at-caret is not implemented yet.
- Cursor movement is UTF-8 aware.
- `WriterWrappedLayout::Line` stores:
  - rendered line text
  - source `startOffset`
  - source `endOffset`
- Writer wrapping now uses measured pixel width via a `MeasureText` callable.
- `WriterActivity` caches the wrapped layout to keep cursor movement fast.
- Simulator text input is in `WriterInput`/`WriterSimInput`; `WriterInput` is a namespace.
- Device-style navigation stays through existing mapped input/button path.

## Constraints And Preferences

User preferences:

- Go slowly, one small slice at a time.
- Prefer one small commit per logical change.
- Explain code and C++ concepts as we go.
- User usually wants the exact git commands and will run them unless explicitly asking Codex to do it.
- Review code before committing; do not auto-commit unless asked.
- Use test-driven development where practical.
- Keep simulator-first development as long as possible.
- Preserve upstream mergeability; avoid broad unrelated refactors.
- Keep changes local to Writer unless there is a strong reason.
- Prefer safe, clean MVP foundations over rushed feature growth.
- Avoid changing SDK/simulator/upstream internals unless clearly necessary.
- Battery life matters; long-term input should be interrupt-driven and allow sleep after inactivity.

Git workflow preferences:

- `master` tracks `origin/master` (user fork).
- `upstream/master` remains the official CrossPoint reference.
- Use PRs against `mahood73/crosspoint-reader`, not upstream.
- Use `--force-with-lease` only for clearly understood branch-history cleanup.
- CodeRabbit is used as a second reviewer, but comments should be verified, not blindly followed.

## Non-negotiables / user preferences

- Runs locally where possible.
- Cloud AI is acceptable, but local files and state should remain under user control.
- Prefer simple, inspectable architecture over clever abstractions.
- Avoid large framework changes unless explicitly approved.
- Keep implementation agent-friendly: clear docs, clear commands, small commits.

## Known Bugs, Failed Approaches, And Gotchas

- GitHub UI can accidentally target upstream PRs. Always check base/head repositories.
- `master` previously tracked `upstream/master`; this was changed because it confused VSCode and push/pull prompts.
- PlatformIO simulator env is Apple Silicon/Homebrew-specific. This is deferred and not a Writer PR blocker.
- Simulator dependency is currently `mahood73/crosspoint-simulator#dev`; pinning to a commit is deferred until simulator work stabilizes.
- `WriterVisibleLines` is effectively dead/unused after cursor-aware viewport work. It is a cleanup candidate, not active rendering path.
- `onExit()` does not flush as a final fallback. Current user-driven exits already flush through Back/Confirm. Revisit when draft switching/menu-driven exits exist.
- `readDraft()` reads only the last 64KB. It aligns the tail window to avoid beginning on a UTF-8 continuation byte, but full-document paging/windowing is deferred.
- First render/open of a large draft can still be slower because wrapping must be computed once.
- Cursor movement was slow after measured wrapping; this was improved by caching the wrapped layout in `WriterActivity`.
- Typing/appending still invalidates the whole layout cache. Paragraph-based caching is a future optimisation.
- Measured wrapping initially used codepoint-by-codepoint measurement for correctness. Word-first measurement is the next likely optimisation.
- `GfxRenderer::wrappedText()` is a useful reference but cannot be used directly because it returns only strings and may truncate with ellipses.
- Writer wrapping must never discard text or insert ellipses.
- `fs_/` is simulator filesystem state and must not be tracked.

## Open Tasks In Priority Order

1. **Open/continue Stage 2 PR**
   - Current branch: `stage-2-writer-wrap`
   - Verify builds/tests before PR if not already done.

2. **Word-first measured wrapping optimisation**
   - Current measured wrapper tests every growing codepoint substring.
   - Optimise normal prose by testing `current line + next word`.
   - Only fall back to codepoint-by-codepoint splitting inside overlong words.
   - Preserve offsets and UTF-8 safety.

3. **Paragraph-based layout caching**
   - Current cache invalidates whole layout on any text change.
   - Future cache could wrap/cache per paragraph to reduce work for appends or local edits.
   - Do not implement until the simpler cache and word-first optimisation have been evaluated.

4. **Insert-at-caret editing**
   - Current text entry appends to end.
   - Cursor movement exists, but typing does not insert at cursor yet.

5. **Keyboard activity and sleep timer**
   - Keyboard input should reset inactivity sleep timer.
   - Long-term real keyboard input should avoid polling for battery life.

6. **Draft management**
   - User-controlled draft names.
   - Multi-file draft selection.
   - Cursor/draft state persistence.

7. **Full-document/windowed storage**
   - Replace 64KB tail-only model with paging/windowing inspired by TXT reader.

8. **Cleanup**
   - Remove/refactor `WriterVisibleLines`.
   - Consider resolving duplicate render text/word count scan.
   - Revisit simulator dependency pinning.

## Files Most Relevant To Continue Work

Writer implementation:

- `src/activities/writer/WriterActivity.cpp`
- `src/activities/writer/WriterActivity.h`
- `src/activities/writer/WriterWrappedLayout.cpp`
- `src/activities/writer/WriterWrappedLayout.h`
- `src/activities/writer/WriterCursor.cpp`
- `src/activities/writer/WriterCursor.h`
- `src/activities/writer/WriterDraftStore.cpp`
- `src/activities/writer/WriterDraftStore.h`
- `src/activities/writer/WriterInput.cpp`
- `src/activities/writer/WriterInput.h`
- `src/activities/writer/WriterSimInput.cpp`
- `src/activities/writer/WriterSimInput.h`
- `src/activities/writer/WriterViewport.cpp`
- `src/activities/writer/WriterViewport.h`

Integration:

- `src/activities/ActivityManager.cpp`
- `src/activities/ActivityManager.h`
- `src/activities/home/HomeActivity.cpp`
- `lib/I18n/translations/english.yaml`

Tests:

- `test/writer_wrapped_layout/WriterWrappedLayoutTest.cpp`
- `test/run_writer_wrapped_layout_test.sh`
- `test/writer_cursor/WriterCursorTest.cpp`
- `test/run_writer_cursor_test.sh`
- `test/writer_viewport/WriterViewportTest.cpp`
- `test/run_writer_viewport_test.sh`

Docs:

- `docs/writerdeck/stage-1-status.md`
- `docs/writerdeck/development-flow.md`
- `docs/superpowers/specs/2026-04-21-x4-writerdeck-mvp-design.md`
- `docs/superpowers/specs/2026-04-23-writer-viewport-and-cursor-design.md`
- `docs/superpowers/specs/2026-04-24-writer-pixel-wrapping-design.md`
- `docs/superpowers/plans/2026-04-23-writer-cursor-viewport.md`
- `docs/superpowers/plans/2026-04-24-writer-pixel-wrapping-stage-2a.md`

Reference:

- `lib/GfxRenderer/GfxRenderer.cpp`
  - `GfxRenderer::wrappedText()`
  - `GfxRenderer::truncatedText()`
- `lib/GfxRenderer/GfxRenderer.h`
- `lib/Utf8/Utf8.cpp`
- `lib/Utf8/Utf8.h`

## Preferred Workflow

- Start by checking branch/status:

```bash
git status --short --branch
git log --oneline --decorate -8
```

- Explain what context is being gathered and why.
- For new behavior, discuss the approach before coding.
- Use TDD for helper logic:
  - write failing test
  - run it and confirm failure
  - implement minimal code
  - rerun tests
- For UI/simulator behavior, user often runs simulator and reports.
- Stop before commits unless user asks Codex to commit.
- Provide suggested commit commands/messages.
- Prefer small commits:
  - docs/spec
  - plan
  - test/helper API
  - behavior implementation
  - cleanup/performance
- When reviewing CodeRabbit comments:
  - verify first
  - fix only if it improves MVP safety/quality
  - reply/defer when out of scope

## Assumptions Not To Change Without Asking

- Keep CrossPoint as the base.
- Do not pivot to MicroSlate firmware.
- Do not fork from CrossInk as the main base.
- Do not remove reader functionality to make Writer easier unless memory constraints force that discussion.
- Do not implement Bluetooth hardware input yet; simulator-first remains preferred.
- Do not change sleep policy to always prevent sleep in Writer; battery life matters.
- Do not add web editor, Markdown rendering, or multi-draft UX without a separate design discussion.
- Do not broadly refactor Home, GfxRenderer, SDK, or simulator internals as part of Writer changes unless the user approves.
- Do not assume generated i18n headers need committing; translation YAML is source of truth.
- Do not track simulator `fs_/` contents.
- Do not target PRs at upstream unless explicitly intended.
- Do not rewrite published branch history unless the user understands and approves the reason.
