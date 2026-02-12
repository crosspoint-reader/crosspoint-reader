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
                                        const std::string& headword, const std::string& definition,
                                        int readerFontId, uint8_t orientation,
                                        const std::function<void()>& onBack)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(headword),
        definition(definition),
        readerFontId(readerFontId),
        orientation(orientation),
        onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::string headword;
  std::string definition;
  int readerFontId;
  uint8_t orientation;
  const std::function<void()> onBack;

  std::vector<std::string> wrappedLines;
  int currentPage = 0;
  int linesPerPage = 0;
  int totalPages = 0;
  bool updateRequired = false;

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;

  bool isLandscape() const;
  bool isInverted() const;
  void wrapText();
  void renderScreen();
  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
};
