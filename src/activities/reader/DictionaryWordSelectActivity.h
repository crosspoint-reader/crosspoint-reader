#pragma once
#include <Epub/Page.h>

#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"

class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int fontId, int marginLeft, int marginTop,
                                        const std::string& cachePath, const std::string& nextPageFirstWord = "")
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        fontId(fontId),
        marginLeft(marginLeft),
        marginTop(marginTop),
        cachePath(cachePath),
        nextPageFirstWord(nextPageFirstWord) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct WordInfo {
    std::string text;
    std::string lookupText;
    int16_t screenX;
    int16_t screenY;
    int16_t width;
    int16_t row;
    int continuationIndex;
    int continuationOf;
    WordInfo(const std::string& t, int16_t x, int16_t y, int16_t w, int16_t r)
        : text(t), lookupText(t), screenX(x), screenY(y), width(w), row(r), continuationIndex(-1), continuationOf(-1) {}
  };

  struct Row {
    int16_t yPos;
    std::vector<int> wordIndices;
  };

  std::unique_ptr<Page> page;
  int fontId;
  int marginLeft;
  int marginTop;
  std::string cachePath;
  std::string nextPageFirstWord;

  std::vector<WordInfo> words;
  std::vector<Row> rows;
  int currentRow = 0;
  int currentWordInRow = 0;

  // State for blocking lookup popup
  bool isLookingUp = false;
  int lookupProgress = 0;

  // State for "Search synonyms?" prompt shown when direct lookup fails
  bool isAskingSynonymSearch = false;
  std::string synSearchWord;

  // State for inline suggestions list shown when no direct/stem/synonym match found
  bool isShowingSuggestions = false;
  std::vector<std::string> suggestionWords;
  int suggestionIndex = 0;

  void extractWords();
  void mergeHyphenatedWords();
  void handleNotFound(const std::string& word);
};
