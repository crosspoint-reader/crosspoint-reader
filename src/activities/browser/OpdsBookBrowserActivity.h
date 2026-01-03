#pragma once
#include <OpdsParser.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Displays a list of available books and allows downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState {
    LOADING,      // Fetching OPDS feed
    BOOK_LIST,    // Displaying books
    DOWNLOADING,  // Downloading selected EPUB
    ERROR         // Error state with message
  };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::function<void()>& onGoHome)
      : Activity("OpdsBookBrowser", renderer, mappedInput), onGoHome(onGoHome) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;

  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsBook> books;
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;

  void fetchBooks();
  void downloadBook(const OpdsBook& book);
  std::string sanitizeFilename(const std::string& title) const;
};
