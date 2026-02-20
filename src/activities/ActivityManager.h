#pragma once

#include <cassert>
#include <string>
#include <typeinfo>

#include "GfxRenderer.h"
#include "MappedInputManager.h"

struct Intent {
  std::string path;

  // FullScreenMessage
  std::string message;
  EpdFontFamily::Style messageStyle;
};

class Activity;  // forward declaration

class ActivityManager {
 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  Activity* currentActivity = nullptr;

  void exitActivity();
  void enterNewActivity(Activity* newActivity);

  // Pending activity to be launched on next loop iteration
  Activity* pendingActivity = nullptr;

 public:
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput) {}
  ~ActivityManager() { assert(false); /* should never be called */ };

  void loop();

  void goToFileTransfer();
  void goToSettings();
  void goToMyLibrary(Intent&& intent);
  void goToRecentBooks();
  void goToBrowser();
  void goToReader(Intent&& intent);
  void goToSleep();
  void goToBoot();
  void goToFullScreenMessage(Intent&& intent);
  void goHome();

  bool preventAutoSleep() const;
  bool isReaderActivity() const;
  bool skipLoopDelay() const;
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
