#pragma once

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"

class GenerateAllCoversActivity final : public ActivityWithSubactivity {
 public:
  explicit GenerateAllCoversActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::function<void()>& goBack)
      : ActivityWithSubactivity("GenerateAllCovers", renderer, mappedInput), goBack(goBack) {}

  void onEnter() override;
  void loop() override;
  void onExit() override;

  bool preventAutoSleep() override { return currentState == GENERATING; }

 private:
  const std::function<void()> goBack;

  enum State { SCANNING, GENERATING, COMPLETE, CANCELLED };
  State currentState = SCANNING;

  // Book list
  std::vector<String> epubFiles;
  int currentIndex = 0;
  int totalBooks = 0;

  // Statistics
  int coversGenerated = 0;
  int thumbsGenerated = 0;
  int skipped = 0;
  int failed = 0;

  // UI state
  bool shouldCancel = false;
  unsigned long startTime = 0;
  unsigned long lastRefreshTime = 0;
  String currentBookTitle;

  // Methods
  void scanLibraryForEpubs();
  void scanDirectoryRecursive(const char* path, int depth = 0);
  void generateCoversForBook(const String& epubPath);
  void renderProgress();
  void renderSummary();
  String formatTime(unsigned long milliseconds);
  String truncateFilename(const String& path, int maxLength);
};
