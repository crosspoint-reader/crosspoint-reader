// BleSyncWaitActivity — the BLOCKING book-open / boot-to-book sync wait
// (PROTOCOL-v2.md §5). Shown ONLY when about to open a book that hasn't been
// reconciled recently: it holds the reader off until that book's position is
// current, so the reader opens at the right page with NO surprise mid-read jump.
//
// The user can SKIP at any time (any key) — this is the one place a sync is
// skippable. On finish/skip it opens the target book at the (now-synced) position
// via goToReaderDirect(), which bypasses the gate so we don't re-enter here.
#pragma once

#include <string>

#include "activities/Activity.h"

class BleSyncWaitActivity : public Activity {
 public:
  BleSyncWaitActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                      unsigned long deadlineMs = 12000);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  void openBook();

  std::string bookPath_;
  unsigned long deadlineMs_;
  bool opened_ = false;
};
