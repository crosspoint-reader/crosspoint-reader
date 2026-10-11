#pragma once

#include <BlePageTurner.h>

// The Bluetooth page turner's link note (bleturner::linkNote) in the reader's status bar: it
// stands in for the title while a remote connects, and after a failed link until the next page
// turn. draw() runs where the status bar draws its title (render task, under the render lock),
// repaint() on the main loop under a tried render lock.
struct LinkNoteTitle {
  // The note to show in the title slot, None for the book's own title.
  bleturner::LinkNote draw(const bool titleShown, const bleturner::LinkNote now) {
    drawnInTitle = titleShown;
    drawn = titleShown ? now : bleturner::LinkNote::None;
    return drawn;
  }

  // True, once, when the note changed after the status bar drew it: paint the page again. A paint
  // that drew no title slot is never repainted for the note, so a hidden title cannot loop.
  bool repaint(const bleturner::LinkNote now) {
    if (!drawnInTitle || now == drawn) return false;
    drawnInTitle = false;
    return true;
  }

 private:
  bool drawnInTitle = false;
  bleturner::LinkNote drawn = bleturner::LinkNote::None;
};
