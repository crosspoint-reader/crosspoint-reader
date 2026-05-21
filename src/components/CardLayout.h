#pragma once

#include <array>
#include <functional>
#include <string>
#include <utility>

#include "components/themes/BaseTheme.h"  // for Rect

class GfxRenderer;

// Reusable "boxed card" layout primitive for stat-style screens.
//
// A CardLayout owns the vertical cursor on a content rect and renders a
// stack of rounded-border cards. Each card has an optional title and a body
// composed by the caller via a lambda; the body lambda can call helpers on
// the CardBody it receives to draw label/value rows or a 4-cell stat grid.
//
// Typical usage (Reading Stats screen):
//
//   CardLayout layout(renderer, contentRect, startY);
//   layout.card("Total time", [&](CardLayout::Body& b) {
//     b.rowLR("Sessions", "12");
//     b.rowLR("Pages",    "248");
//   });
//   layout.card(nullptr, [&](CardLayout::Body& b) {
//     b.statGrid({{ {"3", "Sess"}, {"7", "Books"}, {"4", "Streak"}, {"9", "Best"} }});
//   });
//
// Design notes
//   - Title-less cards (title == nullptr) are used as "hero" panels where
//     the grid is itself the headline.
//   - Card height is computed automatically by tracking how far the body
//     lambda advanced its internal y cursor. The rounded rect is drawn
//     LAST so it sits cleanly around the content (it can't clip text
//     because content padding is fixed at construction time).
//   - All values use UI_10 font; grid values use UI_12 bold. This is
//     consistent across stats screens; if a future caller needs another
//     font scale we'll add an optional config struct.
// Card visual / spacing configuration. Lives outside CardLayout so it can
// be default-constructed by callers (GCC won't synthesize a default ctor
// for a class-nested aggregate inside its own class's default-arg list).
struct CardLayoutConfig {
  int radius = 4;
  int innerPadX = 10;
  int innerPadY = 6;
  int titleGap = 4;
  int cardSpacing = 6;
  // Horizontal margin between the content rect and the card's outer
  // bounds. Defaults to 0; pass theme.verticalSpacing * 2 to line up
  // with the rest of the system UI on a given theme.
  int outerMarginX = 0;
};

class CardLayout {
 public:
  using Config = CardLayoutConfig;  // backwards-compat alias for call sites.

  // Inner-card drawing context handed to card body lambdas. Tracks the
  // running y cursor inside the card and exposes the same content geometry
  // (innerLeft / innerRight / innerWidth) needed for custom layouts.
  class Body {
    friend class CardLayout;
    const CardLayout& layout;
    int& innerY;
    Body(const CardLayout& layout, int& innerY) : layout(layout), innerY(innerY) {}

   public:
    int currentY() const { return innerY; }
    void advance(int dy) { innerY += dy; }
    int innerLeft() const { return layout.innerLeft_; }
    int innerRight() const { return layout.innerRight_; }
    int innerWidth() const { return layout.innerWidth_; }
    int lineHeight() const { return layout.lineH_; }
    int rowStep() const { return layout.rowStep_; }

    // Label (bold) left, value right-aligned at innerRight.
    void rowLR(const char* label, const std::string& value);

    // 4-cell stat grid: large bold value above small label, three 1-px
    // dividers between cells. Advances y by ~2.5 line heights.
    void statGrid(const std::array<std::pair<std::string, const char*>, 4>& cells);

    // Centered single-line message (e.g. "No data yet" placeholder).
    void centeredMessage(const char* msg);
  };

  CardLayout(GfxRenderer& renderer, Rect contentRect, int startY, const CardLayoutConfig& cfg = {});

  // Render a single card. `bodyFn` receives a `Body&` and may call its
  // helpers in any order; the card auto-sizes to whatever the body draws.
  // Pass `title == nullptr` for an untitled hero card.
  void card(const char* title, const std::function<void(Body&)>& bodyFn);

  // Where the next card would start. Useful for callers that want to know
  // how much vertical space they've consumed (e.g. to decide whether to
  // skip a later section that wouldn't fit).
  int cursorY() const { return y_; }

 private:
  GfxRenderer& renderer_;
  Rect contentRect_;
  CardLayoutConfig cfg_;

  int cardLeft_;
  int cardWidth_;
  int innerLeft_;
  int innerRight_;
  int innerWidth_;
  int lineH_;
  int rowStep_;
  int titleH_;
  int y_;
};
