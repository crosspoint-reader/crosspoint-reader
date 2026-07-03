#pragma once

// Shared differential-repaint bookkeeping for dictionary word-select rendering.
//
// Both DictionaryDefinitionActivity and DictionaryWordSelectActivity drive the
// same tiny state machine around their highlight repaint: after a full repaint
// that successfully primed the highlight snapshot, the next cursor move may take
// a cheap differential path (restore previous highlight pixels, draw the new
// one, push the panel) instead of re-rendering the whole page. Anything that
// disturbs the framebuffer outside that cycle (controller overlay, multi-select,
// hyphenated wrap, a returning sub-activity) resets back to a full repaint.
//
// The fields and transitions used to be copy-pasted in both activities; this
// consolidates them so they cannot drift apart. It is a plain value member (no
// heap), so it costs nothing beyond the two it replaces.
struct DiffRepaintState {
  enum class Mode { FullPage, Differential };

  Mode nextMode = Mode::FullPage;
  int prevHighlightIdx = -1;

  // True if the next render() is allowed to attempt the differential fast path.
  bool canDifferential() const { return nextMode == Mode::Differential; }

  // The framebuffer was (or is about to be) redrawn from scratch; discard any
  // differential snapshot so the next render goes through the full path.
  void reset() {
    nextMode = Mode::FullPage;
    prevHighlightIdx = -1;
  }

  // Record a completed differential push of the highlight at `currIdx`.
  void recordDifferentialPush(int currIdx) { prevHighlightIdx = currIdx; }

  // Prime state after a full repaint. `snapshotPrimed` is whether the navigator
  // captured a snapshot we can restore next frame; if not, stay on the full path.
  void primeAfterFullRepaint(int currIdx, bool snapshotPrimed) {
    prevHighlightIdx = currIdx;
    nextMode = snapshotPrimed ? Mode::Differential : Mode::FullPage;
  }
};
