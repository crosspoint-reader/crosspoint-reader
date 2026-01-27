#pragma once
#include <Epub/FootnoteEntry.h>

#include <cstring>

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
