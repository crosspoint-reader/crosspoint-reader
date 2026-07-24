#pragma once

class ActivityManager {
 public:
  bool openedFileBrowser = false;

  void goToFileBrowser(const char*) { openedFileBrowser = true; }
};
