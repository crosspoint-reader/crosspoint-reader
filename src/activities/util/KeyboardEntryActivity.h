#pragma once
#include <GfxRenderer.h>

#include <functional>
#include <string>
#include <utility>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Reusable keyboard entry activity for text input.
 * Can be started from any activity that needs text entry via startActivityForResult()
 */
class KeyboardEntryActivity : public Activity {
  struct KeyBlock {
    const char* row[3];
  };
 public:
  /**
   * Constructor
   * @param renderer Reference to the GfxRenderer for drawing
   * @param mappedInput Reference to MappedInputManager for handling input
   * @param title Title to display above the keyboard
   * @param initialText Initial text to show in the input field
   * @param maxLength Maximum length of input text (0 for unlimited)
   * @param isPassword If true, display asterisks instead of actual characters
   */
  explicit KeyboardEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string title = "Enter Text", std::string initialText = "",
                                 const size_t maxLength = 0, const bool isPassword = false)
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        text(std::move(initialText)),
        maxLength(maxLength),
        isPassword(isPassword) {}

  // Activity overrides
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:

  std::string title;
  std::string text;
  size_t maxLength;
  bool isPassword;

  ButtonNavigator buttonNavigator;

  // Keyboard state
  int selectedTopLevel = -1;
  int selectedMidLevel = -1;
  int selectedBottomLevel = -1;
  int shiftState = 0;  // 0 = lower case, 1 = upper case, 2 = shift lock)

  void setLevelOnPress(int level);

  // Handlers
  void onComplete(std::string text);
  void onCancel();

  char getSelectedChar() const;
  bool handleKeyPress();  // false if onComplete was triggered
  int getRowLength(int row) const;

  static KeyBlock keyboard[];
};
