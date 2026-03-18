#include "Activity.h"

#include "ActivityManager.h"

void Activity::onEnter() { LOG_DBG("ACT", "Entering activity: %s", name.c_str()); }

void Activity::onExit() { LOG_DBG("ACT", "Exiting activity: %s", name.c_str()); }

#ifdef HOST_BUILD
void Activity::requestUpdate(bool immediate) { (void)immediate; }

void Activity::requestUpdateAndWait() {}

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  (void)activity;
  this->resultHandler = std::move(resultHandler);
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() {}
#else
void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
#endif
