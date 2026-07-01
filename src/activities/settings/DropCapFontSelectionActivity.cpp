#include "DropCapFontSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Capitals showcase the decorative initial; drop-cap faces are typically used for
// the first (capital) letter of a paragraph. Latin letters, no need to localize.
constexpr const char* DROPCAP_PREVIEW_TEXT = "A B C D E F G";
// Capitals shown in the "Default" preview, integer-scaled like a real drop cap.
constexpr const char* DROPCAP_DEFAULT_SAMPLE = "ABCD";
// Mirror of ChapterHtmlSlimParser's DROPCAP_LINE_SPAN: a default drop cap spans
// this many body lines, which sets the integer scale of the body glyph.
constexpr int DROPCAP_LINE_SPAN = 3;
}  // namespace

DropCapFontSelectionActivity::DropCapFontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const SdCardFontRegistry* registry)
    : Activity("DropCapFontSelect", renderer, mappedInput), registry_(registry) {}

void DropCapFontSelectionActivity::onEnter() {
  Activity::onEnter();

  metrics_ = UITheme::getInstance().getMetrics();
  afterHeader = metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  bottomReserved = metrics_.buttonHintsHeight + metrics_.verticalSpacing;
  usableHeight = renderer.getScreenHeight() - afterHeader - bottomReserved;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;

  entries_.clear();
  entries_.push_back({std::string(), true});  // Default (no dedicated face)
  if (registry_) {
    const auto& families = registry_->getFamilies();
    std::transform(families.begin(), families.end(), std::back_inserter(entries_),
                   [](const SdCardFontFamilyInfo& family) { return Entry{family.name, false}; });
  }

  // Locate the entry matching the saved selection.
  currentIndex_ = 0;
  if (SETTINGS.dropCapFontName[0] != '\0') {
    for (int i = 1; i < static_cast<int>(entries_.size()); i++) {
      if (entries_[i].name == SETTINGS.dropCapFontName) {
        currentIndex_ = i;
        break;
      }
    }
  }

  selectedIndex_ = currentIndex_;
  applyPreview(selectedIndex_);

  requestUpdate();
}

void DropCapFontSelectionActivity::onExit() { Activity::onExit(); }

void DropCapFontSelectionActivity::applyPreview(int index) {
  previewIndex_ = index;
  const bool isDefault = index >= 0 && index < static_cast<int>(entries_.size()) && entries_[index].isDefault;
  if (isDefault) {
    // "Default" has no dedicated face — drop the previewed drop-cap face. The
    // preview is drawn by renderDefaultPreview (scaled body glyph), so no id here.
    sdFontSystem.previewDropCap(renderer, "");
    previewFontId_ = 0;
    return;
  }
  const char* name = (index >= 0 && index < static_cast<int>(entries_.size())) ? entries_[index].name.c_str() : "";
  // Load the face directly (toggle-independent) so the glyphs show even when drop
  // caps are currently off; SETTINGS is left untouched until the user commits.
  previewFontId_ = sdFontSystem.previewDropCap(renderer, name);
}

void DropCapFontSelectionActivity::commitAndFinish() {
  const Entry& entry = entries_[selectedIndex_];
  if (entry.isDefault) {
    SETTINGS.dropCapFontName[0] = '\0';
  } else {
    strncpy(SETTINGS.dropCapFontName, entry.name.c_str(), sizeof(SETTINGS.dropCapFontName) - 1);
    SETTINGS.dropCapFontName[sizeof(SETTINGS.dropCapFontName) - 1] = '\0';
  }
  // Reconcile the live face with the real, toggle-aware settings.
  sdFontSystem.ensureLoaded(renderer);
  finish();
}

void DropCapFontSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // SETTINGS was never modified during preview — just restore the live face.
    sdFontSystem.ensureLoaded(renderer);
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex_ == previewIndex_) {
      commitAndFinish();
    } else {
      applyPreview(selectedIndex_);
      requestUpdate();
    }
    return;
  }

  const int listSize = static_cast<int>(entries_.size());
  const int pageItems =
      UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, previewHeight + metrics_.verticalSpacing);

  buttonNavigator_.onNextRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });
}

int DropCapFontSelectionActivity::drawPreviewLabel(int top, int height, const char* fontName) const {
  const int left = metrics_.previewPadding;
  const int labelFontId = UI_10_FONT_ID;
  const int labelH = renderer.getTextHeight(labelFontId);
  const int labelGap = 4;
  const int labelReserved = labelH + labelGap + metrics_.previewPadding;

  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s\"", tr(STR_PREVIEW), fontName ? fontName : "");
  const int labelY = top + height - metrics_.previewPadding - labelH;
  renderer.drawText(labelFontId, left, labelY, labelBuf);
  return labelReserved;
}

void DropCapFontSelectionActivity::renderPreviewPane(int top, int height, int fontId, const char* fontName) const {
  const int left = metrics_.previewPadding;
  const int width = renderer.getScreenWidth() - (metrics_.previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelReserved = drawPreviewLabel(top, height, fontName);

  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int innerHeight = height - metrics_.previewPadding - labelReserved;
  const int maxLines = std::max(1, innerHeight / (lineH + 2));

  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->prewarmCache(fontId, DROPCAP_PREVIEW_TEXT, 0x01);
  }

  const auto lines = renderer.wrappedText(fontId, DROPCAP_PREVIEW_TEXT, width, maxLines);

  int y = top + metrics_.previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  for (const auto& line : lines) {
    if (y + lineH > textBottomLimit) break;
    renderer.drawText(fontId, left, y, line.c_str());
    y += lineH + 2;
  }
}

void DropCapFontSelectionActivity::renderDefaultPreview(int top, int height) const {
  if (height <= 0) return;
  const int labelReserved = drawPreviewLabel(top, height, tr(STR_DROP_CAP_FONT_DEFAULT));

  // Replicate the reader's no-face path: the chapter initial is the body glyph
  // upscaled by an integer factor sized to span DROPCAP_LINE_SPAN body lines
  // (clamped 2..4) — see ChapterHtmlSlimParser / TextBlock::render.
  const int fontId = SETTINGS.getReaderFontId();
  if (fontId == 0) return;
  const int lineHeight = renderer.getTextHeight(fontId);
  if (lineHeight <= 0) return;

  int penX = metrics_.previewPadding;
  const int paneRight = renderer.getScreenWidth() - metrics_.previewPadding;
  const int glyphTop = top + metrics_.previewPadding;
  const int bottomLimit = top + height - labelReserved;

  for (const char* s = DROPCAP_DEFAULT_SAMPLE; *s; ++s) {
    const uint32_t cp = static_cast<unsigned char>(*s);  // sample is ASCII capitals
    int gw = 0, gh = 0, gtop = 0, gadv = 0;
    if (!renderer.getGlyphBox(fontId, cp, EpdFontFamily::REGULAR, gw, gh, gtop, gadv) || gh <= 0 || gadv <= 0) continue;
    const uint8_t scale = static_cast<uint8_t>(std::clamp((DROPCAP_LINE_SPAN * lineHeight + gh / 2) / gh, 2, 4));
    if (glyphTop + gh * scale > bottomLimit) break;  // would overflow the pane height
    if (penX + gw * scale > paneRight) break;        // would overflow the pane width
    renderer.drawScaledGlyph(fontId, cp, EpdFontFamily::REGULAR, scale, penX, glyphTop, true);
    penX += gadv * scale + scale;  // advance + a small gap between caps
  }
}

void DropCapFontSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, pageWidth, metrics_.headerHeight}, tr(STR_DROP_CAP_FONT));

  const int previewTop = afterHeader;
  const int listTop = previewTop + previewHeight + metrics_.verticalSpacing;
  const int listHeight = usableHeight - previewHeight - metrics_.verticalSpacing;

  const bool previewIsDefault =
      previewIndex_ >= 0 && previewIndex_ < static_cast<int>(entries_.size()) && entries_[previewIndex_].isDefault;
  if (previewIsDefault) {
    renderDefaultPreview(previewTop, previewHeight);
  } else {
    const char* previewName = (previewIndex_ >= 0 && previewIndex_ < static_cast<int>(entries_.size()))
                                  ? entries_[previewIndex_].name.c_str()
                                  : nullptr;
    renderPreviewPane(previewTop, previewHeight, previewFontId_, previewName);
  }

  renderer.drawLine(0, listTop - metrics_.verticalSpacing / 2, pageWidth, listTop - metrics_.verticalSpacing / 2);

  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(entries_.size()), selectedIndex_,
      [this](int index) -> std::string {
        return entries_[index].isDefault ? tr(STR_DROP_CAP_FONT_DEFAULT) : entries_[index].name;
      },
      nullptr, nullptr,
      [this](int index) -> std::string {
        if (index == previewIndex_ && index != currentIndex_) return tr(STR_PREVIEW);
        if (index == currentIndex_) return tr(STR_SELECTED);
        return "";
      },
      true);

  const bool onPreviewed = selectedIndex_ == previewIndex_;
  const char* confirmLabel = onPreviewed ? tr(STR_SELECT) : tr(STR_PREVIEW);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
