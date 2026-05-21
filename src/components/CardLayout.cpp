#include "CardLayout.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <string>

#include "fontIds.h"

CardLayout::CardLayout(GfxRenderer& renderer, Rect contentRect, int startY, const CardLayoutConfig& cfg)
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

namespace {
// Split `label` into 1-2 centered lines that fit within `maxWidth`. Splits
// at the last space whose prefix still fits; if no such split exists (single
// long word) we fall back to character-wise truncation of the original with
// an ellipsis. The two-line variant is preferred whenever both halves fit
// inside the cell, so "Books tracked" wraps rather than getting clipped.
struct WrappedLabel {
  std::string line1;
  std::string line2;  // empty when single-line
};

WrappedLabel wrapLabel(const GfxRenderer& renderer, const char* label, int maxWidth, int fontId) {
  WrappedLabel out;
  if (!label || !*label) return out;
  if (renderer.getTextWidth(fontId, label) <= maxWidth) {
    out.line1 = label;
    return out;
  }

  // Find the last whitespace whose prefix fits and whose suffix also fits.
  const std::string s(label);
  size_t bestSplit = std::string::npos;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != ' ') continue;
    const std::string left = s.substr(0, i);
    const std::string right = s.substr(i + 1);
    if (renderer.getTextWidth(fontId, left.c_str()) <= maxWidth &&
        renderer.getTextWidth(fontId, right.c_str()) <= maxWidth) {
      bestSplit = i;
    }
  }
  if (bestSplit != std::string::npos) {
    out.line1 = s.substr(0, bestSplit);
    out.line2 = s.substr(bestSplit + 1);
    return out;
  }

  // No good split — truncate the single token. Keep dropping characters
  // until label + "…" fits.
  const int ellipsisW = renderer.getTextWidth(fontId, "…");
  std::string truncated = s;
  while (!truncated.empty() && renderer.getTextWidth(fontId, truncated.c_str()) + ellipsisW > maxWidth) {
    truncated.pop_back();
  }
  out.line1 = truncated + "…";
  return out;
}
}  // namespace

void CardLayout::Body::statGrid(const std::array<std::pair<std::string, const char*>, 4>& cells) {
  // Labels use SMALL_FONT_ID so multi-word labels like "Pages turned" /
  // "Books tracked" / "Avg session" usually fit on one line. The two-line
  // wrap below is still the fallback for genuinely long labels.
  constexpr int kLabelFont = SMALL_FONT_ID;
  const int labelLineH = layout.renderer_.getLineHeight(kLabelFont);

  const int cellW = layout.innerWidth_ / 4;
  // Leave a small horizontal breathing room inside each cell so wrapped
  // labels don't kiss the vertical divider on the next cell over.
  constexpr int kLabelPadX = 4;
  const int labelMaxW = std::max(0, cellW - kLabelPadX * 2);

  // Pre-wrap so we can compute the row's vertical extent before drawing the
  // dividers (which span both label lines whenever any cell wraps).
  std::array<WrappedLabel, 4> wrapped;
  bool anyTwoLines = false;
  for (int i = 0; i < 4; ++i) {
    wrapped[i] = wrapLabel(layout.renderer_, cells[i].second, labelMaxW, kLabelFont);
    if (!wrapped[i].line2.empty()) anyTwoLines = true;
  }

  const int valueY = innerY;
  const int labelY = innerY + layout.lineH_ + 2;
  const int labelLine2Y = labelY + labelLineH;
  const int gridBottom = (anyTwoLines ? labelLine2Y : labelY) + labelLineH;

  for (int i = 0; i < 4; ++i) {
    const auto& [value, _label] = cells[i];
    const int cellCenterX = layout.innerLeft_ + cellW * i + cellW / 2;
    const int vw = layout.renderer_.getTextWidth(UI_12_FONT_ID, value.c_str(), EpdFontFamily::BOLD);
    layout.renderer_.drawText(UI_12_FONT_ID, cellCenterX - vw / 2, valueY, value.c_str(), true, EpdFontFamily::BOLD);

    const auto& w = wrapped[i];
    const int lw1 = layout.renderer_.getTextWidth(kLabelFont, w.line1.c_str());
    layout.renderer_.drawText(kLabelFont, cellCenterX - lw1 / 2, labelY, w.line1.c_str());
    if (!w.line2.empty()) {
      const int lw2 = layout.renderer_.getTextWidth(kLabelFont, w.line2.c_str());
      layout.renderer_.drawText(kLabelFont, cellCenterX - lw2 / 2, labelLine2Y, w.line2.c_str());
    }

    if (i < 3) {
      const int divX = layout.innerLeft_ + cellW * (i + 1);
      layout.renderer_.drawLine(divX, valueY - 2, divX, gridBottom, true);
    }
  }
  innerY = gridBottom + 4;
}

void CardLayout::Body::centeredMessage(const char* msg) {
  const int mw = layout.renderer_.getTextWidth(UI_10_FONT_ID, msg);
  layout.renderer_.drawText(UI_10_FONT_ID, layout.innerLeft_ + (layout.innerWidth_ - mw) / 2, innerY, msg);
  innerY += layout.rowStep_;
}
