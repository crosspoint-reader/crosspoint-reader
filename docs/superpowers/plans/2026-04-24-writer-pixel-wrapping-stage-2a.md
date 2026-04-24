# Writer Pixel Wrapping Stage 2A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Writer's approximate codepoint-count wrapping with measured pixel-width wrapping while preserving line byte offsets for cursor movement.

**Architecture:** `WriterWrappedLayout` remains the cursor-aware layout helper. It gains a small width-measuring seam so host tests can use predictable widths while `WriterActivity` uses `GfxRenderer::getTextWidth(...)` in production. The implementation borrows `GfxRenderer::wrappedText()`'s word-growth approach but keeps editor-specific offsets and hard-wraps overlong words instead of truncating with ellipses.

**Tech Stack:** C++20, PlatformIO, CrossPoint `GfxRenderer`, `Utf8` helpers, existing standalone shell test runners.

---

## File Structure

- Modify `src/activities/writer/WriterWrappedLayout.h`
  - Add a small measuring type.
  - Change `wrap(...)` to accept `maxWidth` and a measuring function.
  - Keep `Line{text, startOffset, endOffset}` unchanged.
- Modify `src/activities/writer/WriterWrappedLayout.cpp`
  - Replace codepoint-count wrapping with measured-width wrapping.
  - Keep UTF-8-safe offset advancement.
  - Prefer spaces as break points.
  - Hard-wrap oversized words without ellipses.
- Modify `src/activities/writer/WriterActivity.cpp`
  - Pass actual content width and a renderer-backed measuring function to `WriterWrappedLayout::wrap(...)`.
  - Remove `estimateWrapColumns(...)` if no longer used.
- Modify `src/activities/writer/WriterActivity.h`
  - Remove the `estimateWrapColumns(...)` declaration if it is removed from the `.cpp`.
- Modify `test/writer_wrapped_layout/WriterWrappedLayoutTest.cpp`
  - Convert existing tests to the new measuring API.
  - Add pixel-width-specific cases.
- Use existing `test/run_writer_wrapped_layout_test.sh`.

## Task 1: Introduce Width-Measured Wrap API

**Files:**
- Modify: `src/activities/writer/WriterWrappedLayout.h`
- Modify: `src/activities/writer/WriterWrappedLayout.cpp`
- Modify: `test/writer_wrapped_layout/WriterWrappedLayoutTest.cpp`

- [ ] **Step 1: Write the failing API migration test helper**

In `test/writer_wrapped_layout/WriterWrappedLayoutTest.cpp`, add this helper inside the anonymous namespace, after `expectTrue(...)`:

```cpp
int fixedWidthMeasure(const std::string& text) {
  return static_cast<int>(text.size());
}

std::vector<WriterWrappedLayout::Line> wrapWithFixedWidth(const std::string& text, int maxWidth) {
  return WriterWrappedLayout::wrap(text, maxWidth, fixedWidthMeasure);
}
```

Then change each existing test call from:

```cpp
WriterWrappedLayout::wrap(text, 6)
```

or similar to:

```cpp
wrapWithFixedWidth(text, 6)
```

For the string literal case, change:

```cpp
WriterWrappedLayout::wrap("alpha\n\nbeta", 10)
```

to:

```cpp
wrapWithFixedWidth("alpha\n\nbeta", 10)
```

- [ ] **Step 2: Run the test and verify it fails to compile**

Run:

```bash
bash test/run_writer_wrapped_layout_test.sh
```

Expected: compilation fails because `WriterWrappedLayout::wrap(...)` does not yet accept a measuring function.

- [ ] **Step 3: Add the measuring type and API**

In `src/activities/writer/WriterWrappedLayout.h`, replace the class body with:

```cpp
class WriterWrappedLayout {
 public:
  using MeasureText = std::function<int(const std::string& text)>;

  struct Line {
    std::string text;
    size_t startOffset;
    size_t endOffset;
  };

  static std::vector<Line> wrap(const std::string& renderedText, int maxWidth, const MeasureText& measureText);
};
```

Also add the required header include:

```cpp
#include <functional>
```

In `src/activities/writer/WriterWrappedLayout.cpp`, change the public method signature to:

```cpp
std::vector<WriterWrappedLayout::Line> WriterWrappedLayout::wrap(const std::string& renderedText,
                                                                 const int maxWidth,
                                                                 const MeasureText& measureText) {
```

For this task only, keep the existing implementation shape by converting the old `maxColumns` setup to:

```cpp
  std::vector<Line> lines;
  const size_t maxColumns = std::max<size_t>(1, static_cast<size_t>(maxWidth));
  (void)measureText;
```

Leave the rest of the existing wrapping code in place for now.

- [ ] **Step 4: Run the test and verify it passes**

Run:

```bash
bash test/run_writer_wrapped_layout_test.sh
```

Expected: `WriterWrappedLayoutTest passed`.

- [ ] **Step 5: Commit**

```bash
git add src/activities/writer/WriterWrappedLayout.h src/activities/writer/WriterWrappedLayout.cpp test/writer_wrapped_layout/WriterWrappedLayoutTest.cpp
git commit -m "writer: add measured wrapping API"
```

## Task 2: Make Wrapping Use Measured Width

**Files:**
- Modify: `src/activities/writer/WriterWrappedLayout.cpp`
- Modify: `test/writer_wrapped_layout/WriterWrappedLayoutTest.cpp`

- [ ] **Step 1: Add failing measured-width tests**

In `test/writer_wrapped_layout/WriterWrappedLayoutTest.cpp`, add these helpers after `wrapWithFixedWidth(...)`:

```cpp
int weightedMeasure(const std::string& text) {
  int width = 0;
  for (const char ch : text) {
    if (ch == 'W') {
      width += 4;
    } else if (ch == 'i') {
      width += 1;
    } else {
      width += 2;
    }
  }
  return width;
}

std::vector<WriterWrappedLayout::Line> wrapWithWeightedWidth(const std::string& text, int maxWidth) {
  return WriterWrappedLayout::wrap(text, maxWidth, weightedMeasure);
}
```

Then add this test before `preservesBlankLines()`:

```cpp
void wrapsUsingMeasuredWidthNotCharacterCount() {
  const auto lines = wrapWithWeightedWidth("iiii W", 6);

  expectEqual(lines.size(), 2, "wrapsUsingMeasuredWidthNotCharacterCount", "line count");
  expectEqual(lines[0].text, "iiii", "wrapsUsingMeasuredWidthNotCharacterCount", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "wrapsUsingMeasuredWidthNotCharacterCount", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 4, "wrapsUsingMeasuredWidthNotCharacterCount", "line 0 endOffset");
  expectEqual(lines[1].text, "W", "wrapsUsingMeasuredWidthNotCharacterCount", "line 1 text");
  expectEqual(lines[1].startOffset, 5, "wrapsUsingMeasuredWidthNotCharacterCount", "line 1 startOffset");
  expectEqual(lines[1].endOffset, 6, "wrapsUsingMeasuredWidthNotCharacterCount", "line 1 endOffset");
}
```

Add it to `main()` before the existing tests:

```cpp
  wrapsUsingMeasuredWidthNotCharacterCount();
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
bash test/run_writer_wrapped_layout_test.sh
```

Expected: FAIL in `wrapsUsingMeasuredWidthNotCharacterCount`, because the current implementation still treats `maxWidth` as columns and does not hard-wrap the wide word by measured width.

- [ ] **Step 3: Replace paragraph wrapping with measured-width wrapping**

In `src/activities/writer/WriterWrappedLayout.cpp`, change `appendWrappedParagraph(...)` to this full implementation:

```cpp
void appendWrappedParagraph(const std::string& text,
                            size_t paragraphStart,
                            size_t paragraphEnd,
                            int maxWidth,
                            const WriterWrappedLayout::MeasureText& measureText,
                            std::vector<WriterWrappedLayout::Line>& lines) {
  if (paragraphStart == paragraphEnd) {
    lines.push_back({"", paragraphStart, paragraphStart});
    return;
  }

  size_t lineStart = paragraphStart;
  while (lineStart < paragraphEnd) {
    size_t offset = lineStart;
    size_t lastSpace = std::string::npos;
    size_t lastFit = lineStart;

    while (offset < paragraphEnd) {
      const size_t nextOffset = nextCodepointOffset(text, offset);
      if (nextOffset <= offset) {
        break;
      }

      const std::string candidate = text.substr(lineStart, nextOffset - lineStart);
      if (measureText(candidate) > maxWidth) {
        break;
      }

      if (text[offset] == ' ') {
        lastSpace = offset;
      }

      lastFit = nextOffset;
      offset = nextOffset;
    }

    if (offset >= paragraphEnd) {
      lines.push_back(makeLine(text, lineStart, paragraphEnd));
      break;
    }

    if (lastSpace != std::string::npos && lastSpace > lineStart) {
      lines.push_back(makeLine(text, lineStart, lastSpace));
      lineStart = lastSpace + 1;
      continue;
    }

    if (lastFit > lineStart) {
      lines.push_back(makeLine(text, lineStart, lastFit));
      lineStart = lastFit;
      continue;
    }

    const size_t nextOffset = nextCodepointOffset(text, lineStart);
    if (nextOffset <= lineStart) {
      break;
    }
    lines.push_back(makeLine(text, lineStart, nextOffset));
    lineStart = nextOffset;
  }
}
```

Then update `WriterWrappedLayout::wrap(...)` to clamp width and pass the measuring function:

```cpp
std::vector<WriterWrappedLayout::Line> WriterWrappedLayout::wrap(const std::string& renderedText,
                                                                 const int maxWidth,
                                                                 const MeasureText& measureText) {
  std::vector<Line> lines;
  const int wrappedWidth = std::max(1, maxWidth);

  size_t paragraphStart = 0;
  while (paragraphStart <= renderedText.size()) {
    const size_t newline = renderedText.find('\n', paragraphStart);
    const size_t paragraphEnd = newline == std::string::npos ? renderedText.size() : newline;

    appendWrappedParagraph(renderedText, paragraphStart, paragraphEnd, wrappedWidth, measureText, lines);

    if (newline == std::string::npos) {
      break;
    }
    paragraphStart = newline + 1;
  }

  return lines;
}
```

- [ ] **Step 4: Run the test and verify it passes**

Run:

```bash
bash test/run_writer_wrapped_layout_test.sh
```

Expected: `WriterWrappedLayoutTest passed`.

- [ ] **Step 5: Commit**

```bash
git add src/activities/writer/WriterWrappedLayout.cpp test/writer_wrapped_layout/WriterWrappedLayoutTest.cpp
git commit -m "writer: wrap text by measured width"
```

## Task 3: Cover Long Words And UTF-8 With Measured Wrapping

**Files:**
- Modify: `test/writer_wrapped_layout/WriterWrappedLayoutTest.cpp`
- Modify: `src/activities/writer/WriterWrappedLayout.cpp` if tests expose a bug

- [ ] **Step 1: Add long-word and UTF-8 measured-width tests**

In `test/writer_wrapped_layout/WriterWrappedLayoutTest.cpp`, add these tests before `preservesBlankLines()`:

```cpp
void hardWrapsLongWordsWithoutEllipsis() {
  const auto lines = wrapWithFixedWidth("abcdef", 2);

  expectEqual(lines.size(), 3, "hardWrapsLongWordsWithoutEllipsis", "line count");
  expectEqual(lines[0].text, "ab", "hardWrapsLongWordsWithoutEllipsis", "line 0 text");
  expectEqual(lines[1].text, "cd", "hardWrapsLongWordsWithoutEllipsis", "line 1 text");
  expectEqual(lines[2].text, "ef", "hardWrapsLongWordsWithoutEllipsis", "line 2 text");
}

void emitsOversizedCodepointAsSingleLine() {
  const auto lines = wrapWithWeightedWidth("Wii", 2);

  expectEqual(lines.size(), 2, "emitsOversizedCodepointAsSingleLine", "line count");
  expectEqual(lines[0].text, "W", "emitsOversizedCodepointAsSingleLine", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "emitsOversizedCodepointAsSingleLine", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 1, "emitsOversizedCodepointAsSingleLine", "line 0 endOffset");
  expectEqual(lines[1].text, "ii", "emitsOversizedCodepointAsSingleLine", "line 1 text");
}

void keepsUtf8CodepointsWholeWhenMeasured() {
  const std::string text = "\xC3\xA9" "\xC3\xA9" "\xC3\xA9";
  const auto lines = wrapWithFixedWidth(text, 4);

  expectEqual(lines.size(), 2, "keepsUtf8CodepointsWholeWhenMeasured", "line count");
  expectEqual(lines[0].text, "\xC3\xA9" "\xC3\xA9", "keepsUtf8CodepointsWholeWhenMeasured", "line 0 text");
  expectEqual(lines[0].startOffset, 0, "keepsUtf8CodepointsWholeWhenMeasured", "line 0 startOffset");
  expectEqual(lines[0].endOffset, 4, "keepsUtf8CodepointsWholeWhenMeasured", "line 0 endOffset");
  expectEqual(lines[1].text, "\xC3\xA9", "keepsUtf8CodepointsWholeWhenMeasured", "line 1 text");
  expectEqual(lines[1].startOffset, 4, "keepsUtf8CodepointsWholeWhenMeasured", "line 1 startOffset");
  expectEqual(lines[1].endOffset, 6, "keepsUtf8CodepointsWholeWhenMeasured", "line 1 endOffset");
}
```

Add them to `main()` after `wrapsUsingMeasuredWidthNotCharacterCount()`:

```cpp
  hardWrapsLongWordsWithoutEllipsis();
  emitsOversizedCodepointAsSingleLine();
  keepsUtf8CodepointsWholeWhenMeasured();
```

- [ ] **Step 2: Run the test**

Run:

```bash
bash test/run_writer_wrapped_layout_test.sh
```

Expected: `WriterWrappedLayoutTest passed`. If any test fails, fix `WriterWrappedLayout.cpp` without changing the expected behavior.

- [ ] **Step 3: Commit**

```bash
git add test/writer_wrapped_layout/WriterWrappedLayoutTest.cpp src/activities/writer/WriterWrappedLayout.cpp
git commit -m "test: Cover measured writer wrapping edge cases"
```

## Task 4: Wire WriterActivity To Pixel Wrapping

**Files:**
- Modify: `src/activities/writer/WriterActivity.cpp`
- Modify: `src/activities/writer/WriterActivity.h`

- [ ] **Step 1: Update `WriterActivity::render()` to pass measured width**

In `src/activities/writer/WriterActivity.cpp`, replace:

```cpp
  const auto wrappedLines = WriterWrappedLayout::wrap(renderedText, estimateWrapColumns(contentWidth));
```

with:

```cpp
  const auto wrappedLines = WriterWrappedLayout::wrap(renderedText, contentWidth, [this](const std::string& text) {
    return renderer.getTextWidth(UI_10_FONT_ID, text.c_str());
  });
```

- [ ] **Step 2: Update `WriterActivity::moveCursorVertical()` to pass measured width**

In `src/activities/writer/WriterActivity.cpp`, replace:

```cpp
  const auto wrappedLines = WriterWrappedLayout::wrap(renderedText, estimateWrapColumns(contentWidth));
```

inside `moveCursorVertical(...)` with:

```cpp
  const auto wrappedLines = WriterWrappedLayout::wrap(renderedText, contentWidth, [this](const std::string& text) {
    return renderer.getTextWidth(UI_10_FONT_ID, text.c_str());
  });
```

- [ ] **Step 3: Remove `estimateWrapColumns(...)`**

In `src/activities/writer/WriterActivity.cpp`, delete:

```cpp
size_t WriterActivity::estimateWrapColumns(const int contentWidth) const {
  const int glyphWidth = std::max(1, renderer.getTextWidth(UI_10_FONT_ID, "M"));
  return std::max<size_t>(1, static_cast<size_t>(contentWidth / glyphWidth));
}
```

In `src/activities/writer/WriterActivity.h`, delete:

```cpp
  size_t estimateWrapColumns(int contentWidth) const;
```

- [ ] **Step 4: Run focused host tests**

Run:

```bash
bash test/run_writer_wrapped_layout_test.sh
bash test/run_writer_cursor_test.sh
bash test/run_writer_viewport_test.sh
```

Expected:

```text
WriterWrappedLayoutTest passed
WriterCursorTest passed
WriterViewportTest passed
```

- [ ] **Step 5: Run PlatformIO builds**

Run:

```bash
pio run -e simulator
pio run
```

Expected: both builds complete with `SUCCESS`.

- [ ] **Step 6: Manual simulator check**

Run:

```bash
pio run -e simulator -t run_simulator
```

Expected:

- Writer opens from Home.
- A paragraph with many `i` characters wraps later than before.
- A paragraph with many `W` characters wraps earlier than before.
- Caret remains visible while moving left, right, up, and down.
- Footer remains at the bottom and does not overlap text.

- [ ] **Step 7: Commit**

```bash
git add src/activities/writer/WriterActivity.cpp src/activities/writer/WriterActivity.h
git commit -m "writer: use pixel-width wrapping in activity"
```

## Task 5: Final Verification And PR Prep

**Files:**
- Review all Stage 2A changes.

- [ ] **Step 1: Run all writer host tests**

Run:

```bash
bash test/run_writer_cursor_test.sh
bash test/run_writer_wrapped_layout_test.sh
bash test/run_writer_viewport_test.sh
bash test/run_writer_visible_lines_test.sh
```

Expected all pass:

```text
WriterCursorTest passed
WriterWrappedLayoutTest passed
WriterViewportTest passed
WriterVisibleLinesTest passed
```

- [ ] **Step 2: Run build checks**

Run:

```bash
pio run -e simulator
pio run
```

Expected: both builds complete with `SUCCESS`.

- [ ] **Step 3: Inspect diff**

Run:

```bash
git diff master...HEAD --stat
git diff master...HEAD -- src/activities/writer test/writer_wrapped_layout docs/superpowers
```

Expected:

- Stage 2A touches only Writer wrapping/activity code, wrapping tests, and Stage 2 docs.
- No simulator dependency changes.
- No unrelated generated files.

- [ ] **Step 4: Push branch**

```bash
git push -u origin stage-2-writer-wrap
```

Expected: branch is pushed to `origin`.

---

## Self-Review

- Spec coverage: Stage 2A covers pixel-width wrapping, offset preservation, hard-wrapping long words, UTF-8-safe offsets, and simulator-testable wiring. Stage 2B-2D remain documented follow-ups.
- Placeholder scan: no placeholder implementation steps remain.
- Type consistency: the plan uses one new `std::function`-based `MeasureText` API consistently in header, implementation, tests, and activity wiring, so production can pass a renderer-capturing lambda.
