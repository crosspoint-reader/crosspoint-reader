#pragma once
#include <vector>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FlashcardActivity final : public Activity {
 public:
  explicit FlashcardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Flashcard", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { DECK_SELECT, CARD_FRONT, CARD_BACK, STATS };
  State state = DECK_SELECT;

  struct Card {
    char front[128];
    char back[128];
    int correct;
    int wrong;
  };

  std::vector<std::string> deckFiles;
  std::vector<Card> cards;
  int deckIndex = 0;
  int cardIndex = 0;

  static constexpr const char* DECK_DIR = "/biscuit/flashcards";

  void scanDecks();
  bool loadDeck(const std::string& path);
  void renderDeckSelect() const;
  void renderCardFront() const;
  void renderCardBack() const;
  void renderStats() const;
};
