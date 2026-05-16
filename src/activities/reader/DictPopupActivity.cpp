#include "DictPopupActivity.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int PADDING = 10;
constexpr const char* BULLET = "\xe2\x80\xa2";   // "•"
constexpr int BULLET_GAP = 6;                    // gap after bullet
constexpr int POS_GAP = 6;                       // gap after POS label
constexpr int SENSE_GAP = 6;
constexpr int OUTER_MARGIN_X_PCT = 5;
constexpr int MIN_GAP_TO_WORD = 6;

struct PosMap {
  const char* prefix;
  const char* expanded;
};
constexpr PosMap kPosTable[] = {
    {"n. ", "n:"},
    {"v. ", "v:"},
    {"adj. ", "adj:"},
    {"adv. ", "adv:"},
};
}  // namespace

std::vector<std::string> DictPopupActivity::wrapText(const std::string& text, GfxRenderer& renderer, int fontId,
                                                     int firstLineMaxWidth, int continuationMaxWidth) {
  std::vector<std::string> out;
  std::string current;
  std::string tok;
  int maxW = firstLineMaxWidth;
  auto flushTok = [&]() {
    if (tok.empty()) return;
    std::string candidate = current.empty() ? tok : current + " " + tok;
    if (renderer.getTextWidth(fontId, candidate.c_str()) <= maxW) {
      current = candidate;
    } else {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
        maxW = continuationMaxWidth;
      }
      if (renderer.getTextWidth(fontId, tok.c_str()) <= maxW) {
        current = tok;
      } else {
        out.push_back(tok);
      }
    }
    tok.clear();
  };
  for (char c : text) {
    if (c == ' ' || c == '\t') {
      flushTok();
    } else {
      tok += c;
    }
  }
  flushTok();
  if (!current.empty()) out.push_back(current);
  return out;
}

void DictPopupActivity::buildSenses(int maxWidth) {
  const int fontId = SETTINGS.getReaderFontId();
  senses.clear();
  // Split definition by '\n'.
  std::vector<std::string> raw;
  std::string cur;
  for (char c : definition) {
    if (c == '\n') {
      raw.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) raw.push_back(cur);

  const int bulletW = renderer.getTextWidth(fontId, BULLET) + BULLET_GAP;
  for (const auto& line : raw) {
    if (line.empty()) continue;
    Sense s;
    std::string body = line;
    for (const auto& p : kPosTable) {
      size_t plen = std::strlen(p.prefix);
      if (body.size() >= plen && body.compare(0, plen, p.prefix) == 0) {
        s.pos = p.expanded;
        body = body.substr(plen);
        break;
      }
    }
    const int posW = s.pos.empty()
                         ? 0
                         : renderer.getTextWidth(fontId, s.pos.c_str(), EpdFontFamily::ITALIC) + POS_GAP;
    const int firstW = maxWidth - bulletW - posW;
    const int contW = maxWidth - bulletW;
    if (firstW < 40 || contW < 40) {
      s.pos.clear();
      s.bodyLines = wrapText(body, renderer, fontId, maxWidth - bulletW, maxWidth - bulletW);
    } else {
      s.bodyLines = wrapText(body, renderer, fontId, firstW, contW);
    }
    if (s.bodyLines.empty()) s.bodyLines.push_back("");
    senses.push_back(std::move(s));
  }
  sensesBuilt = true;
}

void DictPopupActivity::onEnter() {
  Activity::onEnter();
  needsRender = true;
  scrollLine = 0;
  requestUpdate(true);
}

void DictPopupActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    scrollLine++;
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) && scrollLine > 0) {
    scrollLine--;
    requestUpdate();
  }
}

void DictPopupActivity::render(RenderLock&&) {
  const int fontId = SETTINGS.getReaderFontId();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  const int marginX = screenW * OUTER_MARGIN_X_PCT / 100;
  const int boxX = marginX;
  const int boxW = screenW - 2 * marginX;
  const int innerX = boxX + PADDING;
  const int innerW = boxW - 2 * PADDING;

  if (!sensesBuilt) buildSenses(innerW);

  const int bodyLH = renderer.getLineHeight(fontId);
  const int bulletW = renderer.getTextWidth(fontId, BULLET) + BULLET_GAP;

  // Flatten render rows: each row = (sense_idx, line_idx_within_sense). Lets
  // us measure exact desired height and scroll line-by-line.
  struct Row {
    size_t senseIdx;
    size_t lineIdx;
  };
  std::vector<Row> rows;
  for (size_t si = 0; si < senses.size(); ++si) {
    for (size_t li = 0; li < senses[si].bodyLines.size(); ++li) {
      rows.push_back({si, li});
    }
  }

  // Desired height: rows + inter-sense gaps + padding.
  int senseCount = static_cast<int>(senses.size());
  int interGap = senseCount > 1 ? (senseCount - 1) * SENSE_GAP : 0;
  int desiredBodyH = static_cast<int>(rows.size()) * bodyLH + interGap;
  int boxH = desiredBodyH + 2 * PADDING;

  // Position the box adjacent to the highlighted word.
  const int spaceBelow = screenH - (wordY + wordH) - 2 * MIN_GAP_TO_WORD;
  const int spaceAbove = wordY - 2 * MIN_GAP_TO_WORD;
  const bool placeBelow = spaceBelow >= spaceAbove;
  int maxBoxH = placeBelow ? spaceBelow : spaceAbove;
  if (maxBoxH < bodyLH + 2 * PADDING) maxBoxH = bodyLH + 2 * PADDING;
  if (boxH > maxBoxH) boxH = maxBoxH;

  int boxY;
  if (placeBelow) {
    boxY = wordY + wordH + MIN_GAP_TO_WORD;
    if (boxY + boxH > screenH - MIN_GAP_TO_WORD) {
      boxY = screenH - boxH - MIN_GAP_TO_WORD;
    }
  } else {
    boxY = wordY - MIN_GAP_TO_WORD - boxH;
    if (boxY < MIN_GAP_TO_WORD) boxY = MIN_GAP_TO_WORD;
  }

  // White fill + double black border with a small gap between the lines.
  renderer.fillRect(boxX, boxY, boxW, boxH, false);
  renderer.drawRect(boxX, boxY, boxW, boxH);
  renderer.drawRect(boxX + 3, boxY + 3, boxW - 6, boxH - 6);

  const int bodyTop = boxY + PADDING;
  const int bodyBottom = boxY + boxH - PADDING;
  const int rowCapacity = std::max(1, (bodyBottom - bodyTop + SENSE_GAP) / (bodyLH));
  const int maxScroll = std::max(0, static_cast<int>(rows.size()) - rowCapacity);
  if (scrollLine > maxScroll) scrollLine = maxScroll;

  int y = bodyTop;
  int lastSenseIdx = -1;
  for (size_t i = scrollLine; i < rows.size(); ++i) {
    if (y + bodyLH > bodyBottom) break;
    const Row& r = rows[i];
    const Sense& s = senses[r.senseIdx];
    if (static_cast<int>(r.senseIdx) != lastSenseIdx && lastSenseIdx >= 0) {
      y += SENSE_GAP;
      if (y + bodyLH > bodyBottom) break;
    }
    lastSenseIdx = static_cast<int>(r.senseIdx);
    if (r.lineIdx == 0) {
      renderer.drawText(fontId, innerX, y, BULLET, true);
      int xPos = innerX + bulletW;
      if (!s.pos.empty()) {
        renderer.drawText(fontId, xPos, y, s.pos.c_str(), true, EpdFontFamily::ITALIC);
        xPos += renderer.getTextWidth(fontId, s.pos.c_str(), EpdFontFamily::ITALIC) + POS_GAP;
      }
      renderer.drawText(fontId, xPos, y, s.bodyLines[0].c_str());
    } else {
      renderer.drawText(fontId, innerX + bulletW, y, s.bodyLines[r.lineIdx].c_str());
    }
    y += bodyLH;
  }

  if (maxScroll > 0) {
    char hint[24];
    snprintf(hint, sizeof(hint), "%d/%d", scrollLine + 1, maxScroll + 1);
    const int tw = renderer.getTextWidth(fontId, hint);
    renderer.drawText(fontId, boxX + boxW - PADDING - tw,
                      boxY + boxH - PADDING - bodyLH + 2, hint);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  needsRender = false;
}
