#include "ActivityWithSubactivity.h"

void ActivityWithSubactivity::exitActivity() {
  if (subActivity) {
    subActivity->onExit();
    subActivity.reset();
  }
}

void ActivityWithSubactivity::enterNewActivity(Activity* activity) {
  RenderLock lock(*this);
  subActivity.reset(activity);
  subActivity->onEnter();
}

void ActivityWithSubactivity::loop() {
  if (subActivity) {
    subActivity->loop();
  }
}

void ActivityWithSubactivity::requestUpdate(bool immediate) {
  if (!subActivity) {
    Activity::requestUpdate(immediate);
  }
}

void ActivityWithSubactivity::onExit() {
  exitActivity();
  Activity::onExit();
}
