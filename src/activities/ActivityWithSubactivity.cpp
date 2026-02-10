#include "ActivityWithSubactivity.h"

void ActivityWithSubactivity::exitActivity() {
  // No need to lock, since onExit() already acquires its own lock
  if (subActivity) {
    Serial.printf("[%lu] [ACT] Exiting subactivity...\n", millis());
    subActivity->onExit();
    subActivity.reset();
  }
}

void ActivityWithSubactivity::enterNewActivity(Activity* activity) {
  // Acquire lock to avoid 2 activities rendering at the same time during transition
  RenderLock lock(*this);
  subActivity.reset(activity);
  subActivity->onEnter();
}

void ActivityWithSubactivity::loop() {
  if (subActivity) {
    subActivity->loop();
  }
}

void ActivityWithSubactivity::requestUpdate() {
  if (subActivity) {
    // Note: no need lock here. If we reach this branch, that mean enterNewActivity() must have been called, acquired
    // the lock and make sure there is no pending render requests before passing the control to subActivity
    subActivity->requestUpdate();
  } else {
    Activity::requestUpdate();
  }
}

void ActivityWithSubactivity::onExit() {
  // No need to lock, onExit() already acquires its own lock
  exitActivity();
  Activity::onExit();
}
