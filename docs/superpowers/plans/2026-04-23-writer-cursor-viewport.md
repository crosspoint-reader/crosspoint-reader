# Writer Cursor And Viewport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the first cursor-aware Writer viewport so the user can move through the whole draft with the existing button input path and see a caret rendered at the current insertion point.

**Architecture:** Keep text entry on the existing `WriterInput` path and add movement through the existing button input path. Introduce small pure helpers for UTF-8-aware cursor movement and viewport bookkeeping so `WriterActivity` coordinates behavior without owning all of the math. The first increment stops at button-based navigation, visible caret rendering, and half-page viewport scrolling.

**Tech Stack:** C++20, CrossPoint activity framework, `GfxRenderer`, existing Writer activity code, small standalone C++ test binaries invoked by bash scripts.

---

## File Structure

- Modify: `src/activities/writer/WriterActivity.h`
  - Add editor-state members for cursor and viewport.
- Modify: `src/activities/writer/WriterActivity.cpp`
  - Route button presses into movement helpers, render from viewport, draw a caret.
- Create: `src/activities/writer/WriterCursor.h`
  - Declare UTF-8-aware cursor movement helpers.
- Create: `src/activities/writer/WriterCursor.cpp`
  - Implement left/right movement, line-boundary helpers, and clamping.
- Create: `src/activities/writer/WriterViewport.h`
  - Declare viewport state helpers for keeping the cursor visible.
- Create: `src/activities/writer/WriterViewport.cpp`
  - Implement half-page viewport shifts and top-line clamping.
- Create: `test/writer_cursor/WriterCursorTest.cpp`
  - Exercise UTF-8-aware cursor movement in isolation.
- Create: `test/run_writer_cursor_test.sh`
  - Compile and run the cursor helper test.
- Create: `test/writer_viewport/WriterViewportTest.cpp`
  - Exercise viewport scroll decisions in isolation.
- Create: `test/run_writer_viewport_test.sh`
  - Compile and run the viewport helper test.
- Modify: `docs/writerdeck/stage-1-status.md`
  - Update current limits / likely next steps after the slice lands.

### Task 1: Add UTF-8 Cursor Movement Helpers

**Files:**
- Create: `src/activities/writer/WriterCursor.h`
- Create: `src/activities/writer/WriterCursor.cpp`
- Create: `test/writer_cursor/WriterCursorTest.cpp`
- Create: `test/run_writer_cursor_test.sh`

- [ ] **Step 1: Write the failing cursor helper test**

Create `test/writer_cursor/WriterCursorTest.cpp`:

```cpp
#include <cstdlib>
#include <iostream>
#include <string>

#include "src/activities/writer/WriterCursor.h"

namespace {

void expectEqual(size_t actual, size_t expected, const char* testName) {
  if (actual == expected) return;
  std::cerr << "FAILED: " << testName << " expected " << expected << " got " << actual << "\n";
  std::exit(1);
}

void moveLeftSkipsWholeUtf8Codepoint() {
  const std::string text = "A\xe2\x82\xacB";  // A€B
  size_t cursor = text.size();
  cursor = WriterCursor::moveLeft(text, cursor);
  expectEqual(cursor, 4, "moveLeftSkipsWholeUtf8Codepoint");
}

void moveRightSkipsWholeUtf8Codepoint() {
  const std::string text = "A\xe2\x82\xacB";  // A€B
  size_t cursor = 1;
  cursor = WriterCursor::moveRight(text, cursor);
  expectEqual(cursor, 4, "moveRightSkipsWholeUtf8Codepoint");
}

void clampCursorAtDocumentEdges() {
  const std::string text = "abc";
  expectEqual(WriterCursor::clamp(text, 99), text.size(), "clampCursorAtDocumentEdges");
  expectEqual(WriterCursor::clamp(text, 0), 0, "clampCursorAtDocumentEdgesZero");
}

}  // namespace

int main() {
  moveLeftSkipsWholeUtf8Codepoint();
  moveRightSkipsWholeUtf8Codepoint();
  clampCursorAtDocumentEdges();
  std::cout << "WriterCursorTest passed\n";
  return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
bash test/run_writer_cursor_test.sh
```

Expected: fail with missing `WriterCursor` file or unresolved symbol errors.

- [ ] **Step 3: Add the test runner**

Create `test/run_writer_cursor_test.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/writer_cursor"
BINARY="$BUILD_DIR/WriterCursorTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/writer_cursor/WriterCursorTest.cpp"
  "$ROOT_DIR/src/activities/writer/WriterCursor.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"
"$BINARY" "$@"
```

- [ ] **Step 4: Write the minimal cursor helper implementation**

Create `src/activities/writer/WriterCursor.h`:

```cpp
#pragma once

#include <cstddef>
#include <string>

class WriterCursor {
 public:
  static size_t clamp(const std::string& text, size_t cursor);
  static size_t moveLeft(const std::string& text, size_t cursor);
  static size_t moveRight(const std::string& text, size_t cursor);
};
```

Create `src/activities/writer/WriterCursor.cpp`:

```cpp
#include "WriterCursor.h"

#include <Utf8.h>

namespace {

size_t previousCodepointStart(const std::string& text, size_t cursor) {
  if (cursor == 0) return 0;
  size_t i = cursor - 1;
  while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
    --i;
  }
  return i;
}

size_t nextCodepointEnd(const std::string& text, size_t cursor) {
  if (cursor >= text.size()) return text.size();
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text.data() + cursor);
  utf8NextCodepoint(&ptr);
  return static_cast<size_t>(ptr - reinterpret_cast<const unsigned char*>(text.data()));
}

}  // namespace

size_t WriterCursor::clamp(const std::string& text, size_t cursor) { return std::min(cursor, text.size()); }

size_t WriterCursor::moveLeft(const std::string& text, size_t cursor) {
  return previousCodepointStart(text, clamp(text, cursor));
}

size_t WriterCursor::moveRight(const std::string& text, size_t cursor) {
  return nextCodepointEnd(text, clamp(text, cursor));
}
```

- [ ] **Step 5: Run the cursor helper test to verify it passes**

Run:

```bash
bash test/run_writer_cursor_test.sh
```

Expected: `WriterCursorTest passed`

- [ ] **Step 6: Commit**

```bash
git add test/run_writer_cursor_test.sh test/writer_cursor/WriterCursorTest.cpp src/activities/writer/WriterCursor.h src/activities/writer/WriterCursor.cpp
git commit -m "writer: add cursor movement helpers"
```

### Task 2: Add Viewport Scroll Helpers

**Files:**
- Create: `src/activities/writer/WriterViewport.h`
- Create: `src/activities/writer/WriterViewport.cpp`
- Create: `test/writer_viewport/WriterViewportTest.cpp`
- Create: `test/run_writer_viewport_test.sh`

- [ ] **Step 1: Write the failing viewport helper test**

Create `test/writer_viewport/WriterViewportTest.cpp`:

```cpp
#include <cstdlib>
#include <iostream>

#include "src/activities/writer/WriterViewport.h"

namespace {

void expectEqual(int actual, int expected, const char* testName) {
  if (actual == expected) return;
  std::cerr << "FAILED: " << testName << " expected " << expected << " got " << actual << "\n";
  std::exit(1);
}

void scrollsUpByHalfPageWhenCursorMovesAboveViewport() {
  WriterViewport::State state{10};
  WriterViewport::ensureCursorVisible(state, 8, 10, 4, 100);
  expectEqual(state.topLine, 6, "scrollsUpByHalfPageWhenCursorMovesAboveViewport");
}

void scrollsDownByHalfPageWhenCursorMovesBelowViewport() {
  WriterViewport::State state{10};
  WriterViewport::ensureCursorVisible(state, 21, 10, 4, 100);
  expectEqual(state.topLine, 14, "scrollsDownByHalfPageWhenCursorMovesBelowViewport");
}

void clampsViewportAtTop() {
  WriterViewport::State state{1};
  WriterViewport::ensureCursorVisible(state, 0, 10, 4, 100);
  expectEqual(state.topLine, 0, "clampsViewportAtTop");
}

}  // namespace

int main() {
  scrollsUpByHalfPageWhenCursorMovesAboveViewport();
  scrollsDownByHalfPageWhenCursorMovesBelowViewport();
  clampsViewportAtTop();
  std::cout << "WriterViewportTest passed\n";
  return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
bash test/run_writer_viewport_test.sh
```

Expected: fail with missing `WriterViewport` file or unresolved symbol errors.

- [ ] **Step 3: Add the viewport test runner**

Create `test/run_writer_viewport_test.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/writer_viewport"
BINARY="$BUILD_DIR/WriterViewportTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/writer_viewport/WriterViewportTest.cpp"
  "$ROOT_DIR/src/activities/writer/WriterViewport.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"
"$BINARY" "$@"
```

- [ ] **Step 4: Write the minimal viewport helper implementation**

Create `src/activities/writer/WriterViewport.h`:

```cpp
#pragma once

class WriterViewport {
 public:
  struct State {
    int topLine = 0;
  };

  static void ensureCursorVisible(State& state, int cursorLine, int maxVisibleLines, int scrollStep, int totalLines);
};
```

Create `src/activities/writer/WriterViewport.cpp`:

```cpp
#include "WriterViewport.h"

#include <algorithm>

void WriterViewport::ensureCursorVisible(State& state, int cursorLine, int maxVisibleLines, int scrollStep, int totalLines) {
  const int maxTopLine = std::max(0, totalLines - maxVisibleLines);

  if (cursorLine < state.topLine) {
    state.topLine = std::max(0, cursorLine - scrollStep / 2);
  } else if (cursorLine >= state.topLine + maxVisibleLines) {
    state.topLine = std::min(maxTopLine, cursorLine - maxVisibleLines + 1 - scrollStep / 2);
  }

  state.topLine = std::clamp(state.topLine, 0, maxTopLine);
}
```

- [ ] **Step 5: Run the viewport helper test to verify it passes**

Run:

```bash
bash test/run_writer_viewport_test.sh
```

Expected: `WriterViewportTest passed`

- [ ] **Step 6: Commit**

```bash
git add test/run_writer_viewport_test.sh test/writer_viewport/WriterViewportTest.cpp src/activities/writer/WriterViewport.h src/activities/writer/WriterViewport.cpp
git commit -m "writer: add viewport helpers"
```

### Task 3: Teach WriterActivity About Cursor State

**Files:**
- Modify: `src/activities/writer/WriterActivity.h`
- Modify: `src/activities/writer/WriterActivity.cpp`

- [ ] **Step 1: Add editor-state members in the header**

Update `src/activities/writer/WriterActivity.h`:

```cpp
  std::string draftText;
  std::string inputBuffer;
  size_t cursorIndex = 0;
  int viewportTopLine = 0;
  bool showSaveError = false;
```

Also add helper declarations:

```cpp
  void moveCursorLeft();
  void moveCursorRight();
  std::string& getEditableText();
  const std::string& getEditableText() const;
```

- [ ] **Step 2: Initialize cursor and viewport on entry and flush**

In `src/activities/writer/WriterActivity.cpp`, after reading the draft on enter:

```cpp
  cursorIndex = draftText.size();
  viewportTopLine = 0;
```

After a successful flush:

```cpp
  cursorIndex = draftText.size();
```

- [ ] **Step 3: Implement minimal movement handlers**

Add includes:

```cpp
#include "WriterCursor.h"
```

Add helpers:

```cpp
std::string& WriterActivity::getEditableText() { return draftText; }

const std::string& WriterActivity::getEditableText() const { return draftText; }

void WriterActivity::moveCursorLeft() { cursorIndex = WriterCursor::moveLeft(getRenderedText(), cursorIndex); }

void WriterActivity::moveCursorRight() { cursorIndex = WriterCursor::moveRight(getRenderedText(), cursorIndex); }
```

- [ ] **Step 4: Route existing button input into cursor movement**

In `loop()`, before Back/Confirm handling, add:

```cpp
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    moveCursorLeft();
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    moveCursorRight();
    requestUpdate();
  }
```

For this task, leave `Up` / `Down` unimplemented until the render path can calculate visual lines.

- [ ] **Step 5: Keep text insertion end-only for the first pass**

Leave the current text input code appending to `inputBuffer`:

```cpp
        inputBuffer.push_back(ch);
```

Do not insert at `cursorIndex` yet. This keeps the task focused on navigation state and render groundwork.

- [ ] **Step 6: Commit**

```bash
git add src/activities/writer/WriterActivity.h src/activities/writer/WriterActivity.cpp
git commit -m "writer: add cursor state"
```

### Task 4: Render From Viewport And Draw Caret

**Files:**
- Modify: `src/activities/writer/WriterActivity.cpp`
- Modify: `src/activities/writer/WriterActivity.h`
- Modify: `src/activities/writer/WriterVisibleLines.h`
- Modify: `src/activities/writer/WriterVisibleLines.cpp`

- [ ] **Step 1: Extend the render helper to keep mapping data**

Add a small line record in `WriterActivity.h`:

```cpp
  struct VisibleLine {
    std::string text;
    int documentLine = 0;
  };
```

And replace `std::vector<std::string>` usage in `render()` with `std::vector<VisibleLine>`.

- [ ] **Step 2: Build wrapped visible lines with document line numbers**

In `render()`, replace the tail-following call pattern with a full wrapped-line build:

```cpp
  std::vector<VisibleLine> wrappedLines;
```

For each wrapped line pushed, store:

```cpp
  wrappedLines.push_back(VisibleLine{lineText, currentDocumentLine});
```

Do not trim from the head anymore in this task; keep the full wrapped representation so viewport math can operate on it.

- [ ] **Step 3: Slice the visible page from `viewportTopLine`**

After building `wrappedLines`, compute:

```cpp
  const int visibleStart = std::clamp(viewportTopLine, 0, std::max(0, static_cast<int>(wrappedLines.size()) - 1));
  const int visibleEnd = std::min(static_cast<int>(wrappedLines.size()), visibleStart + maxVisibleLines);
```

Then render only `[visibleStart, visibleEnd)`.

- [ ] **Step 4: Draw a simple caret**

Add a temporary caret rendering pass after drawing text:

```cpp
  const int caretX = x;
  const int caretY = textTop + metrics.verticalSpacing;
  renderer.drawLine(caretX, caretY, caretX, caretY + lineHeight - 2, BLACK);
```

This is intentionally minimal for the first render checkpoint: prove the caret is visible before making its horizontal placement fully cursor-aware.

- [ ] **Step 5: Update the visible-lines helper ownership**

If `WriterVisibleLines` is no longer used after the viewport render rewrite, remove its include from `WriterActivity.cpp`. Do not delete the helper yet until the new render path is proven.

- [ ] **Step 6: Commit**

```bash
git add src/activities/writer/WriterActivity.h src/activities/writer/WriterActivity.cpp src/activities/writer/WriterVisibleLines.h src/activities/writer/WriterVisibleLines.cpp
git commit -m "writer: render first viewport caret"
```

### Task 5: Add Vertical Movement And Viewport Clamping

**Files:**
- Modify: `src/activities/writer/WriterActivity.cpp`
- Modify: `src/activities/writer/WriterActivity.h`
- Modify: `docs/writerdeck/stage-1-status.md`

- [ ] **Step 1: Add viewport helper include**

In `src/activities/writer/WriterActivity.cpp`:

```cpp
#include "WriterViewport.h"
```

- [ ] **Step 2: Move viewport on Up / Down**

In `loop()`, add:

```cpp
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    viewportTopLine = std::max(0, viewportTopLine - 1);
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    viewportTopLine += 1;
    requestUpdate();
  }
```

This keeps the first vertical slice intentionally simple: scrolling the viewport before true cursor-line movement arrives.

- [ ] **Step 3: Clamp the viewport during render**

After computing `wrappedLines.size()` in `render()`:

```cpp
  WriterViewport::State viewport{viewportTopLine};
  WriterViewport::ensureCursorVisible(viewport, viewportTopLine, maxVisibleLines, maxVisibleLines / 2, static_cast<int>(wrappedLines.size()));
  viewportTopLine = viewport.topLine;
```

For this first slice, this is mostly a safe clamp and a place to hook future cursor-aware vertical movement.

- [ ] **Step 4: Update the stage status note**

In `docs/writerdeck/stage-1-status.md`, replace the “Likely Next Steps” bullets with:

```md
- Move from viewport scrolling to true cursor-aware vertical navigation.
- Insert and delete text at the cursor rather than only at the tail.
- Count real keyboard events as activity so sleep policy matches writing behavior.
- Revisit large-document loading once the editor model needs more than an in-memory buffer.
```

- [ ] **Step 5: Manual verification**

Run:

```bash
bash test/run_writer_cursor_test.sh
bash test/run_writer_viewport_test.sh
```

Then in the simulator:

- enter Writer
- verify Left / Right move without crashes
- verify Up / Down scroll the viewport
- verify Escape still exits cleanly
- verify Confirm still flushes buffered text

- [ ] **Step 6: Commit**

```bash
git add src/activities/writer/WriterActivity.cpp src/activities/writer/WriterActivity.h docs/writerdeck/stage-1-status.md
git commit -m "writer: add first viewport navigation"
```

## Self-Review

- Spec coverage: this plan covers the first implementation slice from the spec: button-driven movement, caret introduction, and viewport behavior. It intentionally defers insert-at-cursor editing, long-press movement, and full keyboard navigation keys.
- Placeholder scan: no `TODO`/`TBD` placeholders remain.
- Type consistency: `cursorIndex`, `viewportTopLine`, `WriterCursor`, and `WriterViewport::State` are used consistently across tasks.
