#include "Activity.h"

#include "ActivityManager.h"

void Activity::onEnter() {
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  logSerial.print("[DBG] [ACT] Entering activity: ");
  logSerial.println(name.c_str());
#endif
}

void Activity::onExit() {
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  logSerial.print("[DBG] [ACT] Exiting activity: ");
  logSerial.println(name.c_str());
#endif
}

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

void Activity::onGoHome() { activityManager.goHome(); }

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
