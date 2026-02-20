#include "ActivityManager.h"

#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/MyLibraryActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/SettingsActivity.h"
#include "util/FullScreenMessageActivity.h"

void ActivityManager::exitActivity() {
  if (currentActivity) {
    currentActivity->onExit();
    delete currentActivity;
    currentActivity = nullptr;
  }
}

void ActivityManager::enterNewActivity(Activity* newActivity) {
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity leading to the "delete
    // this" problem
    pendingActivity = newActivity;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = newActivity;
    currentActivity->onEnter();
  }
}

void ActivityManager::loop() {
  if (currentActivity) {
    currentActivity->loop();
  }

  if (pendingActivity) {
    // Current activity has requested a new activity to be launched
    exitActivity();
    currentActivity = pendingActivity;
    pendingActivity = nullptr;
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() { enterNewActivity(new CrossPointWebServerActivity(renderer, mappedInput)); }

void ActivityManager::goToSettings() { enterNewActivity(new SettingsActivity(renderer, mappedInput)); }

void ActivityManager::goToMyLibrary(Intent&& intent) {
  enterNewActivity(new MyLibraryActivity(renderer, mappedInput, intent.path));
}

void ActivityManager::goToRecentBooks() { enterNewActivity(new RecentBooksActivity(renderer, mappedInput)); }

void ActivityManager::goToBrowser() { enterNewActivity(new OpdsBookBrowserActivity(renderer, mappedInput)); }

void ActivityManager::goToReader(Intent&& intent) {
  enterNewActivity(new ReaderActivity(renderer, mappedInput, intent.path));
}

void ActivityManager::goToSleep() { enterNewActivity(new SleepActivity(renderer, mappedInput)); }

void ActivityManager::goToBoot() { enterNewActivity(new BootActivity(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(Intent&& intent) {
  enterNewActivity(new FullScreenMessageActivity(renderer, mappedInput, intent.message, intent.messageStyle));
}

void ActivityManager::goHome() { enterNewActivity(new HomeActivity(renderer, mappedInput)); }

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::isReaderActivity() const { return currentActivity && currentActivity->isReaderActivity(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }
