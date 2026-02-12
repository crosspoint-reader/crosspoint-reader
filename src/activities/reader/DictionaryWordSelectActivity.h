#pragma once
#include <Epub/Page.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"

class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int fontId, int marginLeft, int marginTop,
                                        const std::string& cachePath, uint8_t orientation,
                                        const std::function<void()>& onBack,
                                        const std::function<void(const std::string&, const std::string&)>& onLookup)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        fontId(fontId),
        marginLeft(marginLeft),
        marginTop(marginTop),
        cachePath(cachePath),
        orientation(orientation),
        onBack(onBack),
        onLookup(onLookup) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  struct WordInfo {
    std::string text;
    std::string lookupText;
    int16_t screenX;
    int16_t screenY;
    int16_t width;
    int16_t row;
    int continuationIndex;
    WordInfo(const std::string& t, int16_t x, int16_t y, int16_t w, int16_t r)
        : text(t), lookupText(t), screenX(x), screenY(y), width(w), row(r), continuationIndex(-1) {}
  };

  struct Row {
    int16_t yPos;
    std::vector<int> wordIndices;
  };

  static constexpr int SIDE_HINT_WIDTH = 14;

  std::unique_ptr<Page> page;
  int fontId;
  int marginLeft;
  int marginTop;
  std::string cachePath;
  uint8_t orientation;
  const std::function<void()> onBack;
  const std::function<void(const std::string&, const std::string&)> onLookup;

  std::vector<WordInfo> words;
  std::vector<Row> rows;
  int currentRow = 0;
  int currentWordInRow = 0;
  bool updateRequired = false;

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  bool isLandscape() const;
  bool isInverted() const;
  void extractWords();
  void mergeHyphenatedWords();
  void renderScreen();
  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
};
