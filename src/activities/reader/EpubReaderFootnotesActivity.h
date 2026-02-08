#pragma once
#include <cstring>
#include <functional>
#include <memory>

#include "../../lib/Epub/Epub/FootnoteEntry.h"
#include "../Activity.h"

class FootnotesData {
 private:
  FootnoteEntry entries[16];
  int count;

 public:
  FootnotesData() : count(0) {
    for (int i = 0; i < 16; i++) {
      entries[i].number[0] = '\0';
      entries[i].href[0] = '\0';
    }
  }

  void addFootnote(const char* number, const char* href) {
    if (count < 16 && number && href) {
      strncpy(entries[count].number, number, 2);
      entries[count].number[2] = '\0';
      strncpy(entries[count].href, href, 63);
      entries[count].href[63] = '\0';
      count++;
    }
  }

  void clear() {
    count = 0;
    for (int i = 0; i < 16; i++) {
      entries[i].number[0] = '\0';
      entries[i].href[0] = '\0';
    }
  }

  int getCount() const { return count; }

  const FootnoteEntry* getEntry(int index) const {
    if (index >= 0 && index < count) {
      return &entries[index];
    }
    return nullptr;
  }
};

class EpubReaderFootnotesActivity final : public Activity {
  const FootnotesData& footnotes;
  const std::function<void()> onGoBack;
  const std::function<void(const char*)> onSelectFootnote;
  int selectedIndex;

 public:
  EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const FootnotesData& footnotes,
                              const std::function<void()>& onGoBack,
                              const std::function<void(const char*)>& onSelectFootnote)
      : Activity("EpubReaderFootnotes", renderer, mappedInput),
        footnotes(footnotes),
        onGoBack(onGoBack),
        onSelectFootnote(onSelectFootnote),
        selectedIndex(0) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void render();
};