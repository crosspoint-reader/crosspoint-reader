#include "CardLayout.h"

#include <GfxRenderer.h>

#include "fontIds.h"

CardLayout::CardLayout(GfxRenderer& renderer, Rect contentRect, int startY, Config cfg)
    : renderer_(renderer), contentRect_(contentRect), cfg_(cfg), y_(startY) {
  cardLeft_ = contentRect.x + cfg_.outerMarginX;
  cardWidth_ = contentRect.width - cfg_.outerMarginX * 2;
  innerLeft_ = cardLeft_ + cfg_.innerPadX;
  innerRight_ = cardLeft_ + cardWidth_ - cfg_.innerPadX;
  innerWidth_ = innerRight_ - innerLeft_;
  lineH_ = renderer_.getLineHeight(UI_10_FONT_ID);
  rowStep_ = lineH_ + 2;
  titleH_ = lineH_ + 2;
}

void CardLayout::card(const char* title, const std::function<void(Body&)>& bodyFn) {
  const int top = y_;
  const int titleBlock = title ? titleH_ + cfg_.titleGap : 0;
  const int bodyTop = top + cfg_.innerPadY + titleBlock;
  int innerY = bodyTop;

  // Body draws directly into the frame buffer; we measure how far it
  // advanced so the rounded border can be sized exactly to the content.
  Body body(*this, innerY);
  bodyFn(body);

  const int bodyHeight = innerY - bodyTop;
  const int cardHeight = cfg_.innerPadY + titleBlock + bodyHeight + cfg_.innerPadY;
  renderer_.drawRoundedRect(cardLeft_, top, cardWidth_, cardHeight, 1, cfg_.radius, true);
  if (title) {
    renderer_.drawCenteredText(UI_10_FONT_ID, top + cfg_.innerPadY, title, true, EpdFontFamily::BOLD);
  }
  y_ = top + cardHeight + cfg_.cardSpacing;
}

void CardLayout::Body::rowLR(const char* label, const std::string& value) {
  layout.renderer_.drawText(UI_10_FONT_ID, layout.innerLeft_, innerY, label, true, EpdFontFamily::BOLD);
  const int vw = layout.renderer_.getTextWidth(UI_10_FONT_ID, value.c_str());
  layout.renderer_.drawText(UI_10_FONT_ID, layout.innerRight_ - vw, innerY, value.c_str());
  innerY += layout.rowStep_;
}

void CardLayout::Body::statGrid(const std::array<std::pair<std::string, const char*>, 4>& cells) {
  const int cellW = layout.innerWidth_ / 4;
  const int valueY = innerY;
  const int labelY = innerY + layout.lineH_ + 2;
  for (int i = 0; i < 4; ++i) {
    const auto& [value, label] = cells[i];
    const int cellCenterX = layout.innerLeft_ + cellW * i + cellW / 2;
    const int vw = layout.renderer_.getTextWidth(UI_12_FONT_ID, value.c_str(), EpdFontFamily::BOLD);
    const int lw = layout.renderer_.getTextWidth(UI_10_FONT_ID, label);
    layout.renderer_.drawText(UI_12_FONT_ID, cellCenterX - vw / 2, valueY, value.c_str(), true, EpdFontFamily::BOLD);
    layout.renderer_.drawText(UI_10_FONT_ID, cellCenterX - lw / 2, labelY, label);
    if (i < 3) {
      const int divX = layout.innerLeft_ + cellW * (i + 1);
      layout.renderer_.drawLine(divX, valueY - 2, divX, labelY + layout.lineH_, true);
    }
  }
  innerY = labelY + layout.lineH_ + 4;
}

void CardLayout::Body::centeredMessage(const char* msg) {
  const int mw = layout.renderer_.getTextWidth(UI_10_FONT_ID, msg);
  layout.renderer_.drawText(UI_10_FONT_ID, layout.innerLeft_ + (layout.innerWidth_ - mw) / 2, innerY, msg);
  innerY += layout.rowStep_;
}
