#include "ActivityWithSubactivity.h"

void ActivityWithSubactivity::exitActivity() {
  if (subActivity) {
    subActivity->onExit();
    subActivity.reset();
  }
}

void ActivityWithSubactivity::enterNewActivity(Activity* activity) {
  subActivity.reset(activity);
  subActivity->onEnter();
}

void ActivityWithSubactivity::loop() {
  if (subActivity) {
    subActivity->loop();
  }
}

void ActivityWithSubactivity::onExit() {
  Activity::onExit();
  exitActivity();
}
