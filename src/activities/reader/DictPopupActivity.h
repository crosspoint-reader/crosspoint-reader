#pragma once

#include <string>
#include <vector>

#include "../Activity.h"

// Modal popup showing a dictionary definition for a single word.
// Renders inside a centered box; Back / Confirm dismiss.
class DictPopupActivity final : public Activity {
  struct Sense {
    std::string pos;                    // "noun: " / "verb: " / "" — rendered italic
    std::vector<std::string> bodyLines; // line 0 sits right after pos, rest indent under bullet
  };

  std::string definition;
  std::vector<Sense> senses;
  int wordY = 0;
  int wordH = 0;
  int scrollLine = 0;
  bool needsRender = true;
  bool sensesBuilt = false;

  void buildSenses(int maxWidth);
  static std::vector<std::string> wrapText(const std::string& text, GfxRenderer& renderer, int fontId,
                                           int firstLineMaxWidth, int continuationMaxWidth);

 public:
  explicit DictPopupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             std::string /*unused headword*/, std::string definitionIn,
                             int wordYIn, int wordHIn)
      : Activity("DictPopup", renderer, mappedInput),
        definition(std::move(definitionIn)),
        wordY(wordYIn),
        wordH(wordHIn) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
