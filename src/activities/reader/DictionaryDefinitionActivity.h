#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::string& headword, const std::string& definition, int readerFontId,
                                        const std::function<void()>& onBack,
                                        const std::function<void()>& onDone = nullptr)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(headword),
        definition(definition),
        readerFontId(readerFontId),
        onBack(onBack),
        onDone(onDone) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::string headword;
  std::string definition;
  int readerFontId;
  const std::function<void()> onBack;
  const std::function<void()> onDone;

  std::vector<std::string> wrappedLines;
  int currentPage = 0;
  int linesPerPage = 0;
  int totalPages = 0;
  bool updateRequired = false;

  // Orientation-aware layout gutters (computed in wrapText, used in renderScreen)
  int leftPadding = 20;
  int rightPadding = 20;
  int hintGutterHeight = 0;
  int contentX = 0;
  int hintGutterWidth = 0;

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  void wrapText();
  void renderScreen();
  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
};
