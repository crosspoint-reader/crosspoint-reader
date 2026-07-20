#include "ClipSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <string>
#include <string_view>

#include "clippings/ClippingCodec.h"
#include "clippings/ClippingPageTools.h"
#include "components/UITheme.h"

namespace {

// Returns the byte count of the supported Unicode whitespace at `offset`, or
// zero for a visible codepoint. ClipTextBuilder recognizes the same set.
size_t whitespaceBytesAt(const std::string_view text, const size_t offset, bool* isEmSpace = nullptr) {
  if (isEmSpace) *isEmSpace = false;
  const uint8_t first = static_cast<uint8_t>(text[offset]);
  if (first < 0x80) return std::isspace(first) ? 1 : 0;
  if (first == 0xC2 && offset + 1 < text.size() && static_cast<uint8_t>(text[offset + 1]) == 0xA0) return 2;
  if (first == 0xE2 && offset + 2 < text.size() && static_cast<uint8_t>(text[offset + 1]) == 0x80) {
    const uint8_t last = static_cast<uint8_t>(text[offset + 2]);
    if (last >= 0x80 && last <= 0x8A) {
      if (isEmSpace) *isEmSpace = last == 0x83;
      return 3;
    }
    if (last == 0xAF) return 3;
  }
  return 0;
}

bool startsParagraph(const std::string_view text) {
  for (size_t offset = 0; offset < text.size();) {
    bool isEmSpace = false;
    const size_t whitespaceBytes = whitespaceBytesAt(text, offset, &isEmSpace);
    if (whitespaceBytes == 0) return false;
    if (isEmSpace) return true;
    offset += whitespaceBytes;
  }
  return false;
}

}  // namespace

void ClipSelectionActivity::onEnter() {
  Activity::onEnter();
  if (!page_ || fontId_ == 0 || sectionPageCount_ == 0 || sectionPage_ >= sectionPageCount_) {
    cancel();
    return;
  }

  lineHeight_ = renderer.getLineHeight(fontId_);
  if (lineHeight_ <= 0) {
    cancel();
    return;
  }

  pageFingerprint_ = ClippingPageTools::fingerprint(*page_, renderer, fontId_, marginLeft_, marginTop_);

  extractWords();
  if (readingOrder_.empty()) {
    cancel();
    return;
  }

  // Start near the visual centre so both the beginning and end of the page
  // are reachable without walking through the whole page.
  const int initial = closestOrderInRow(static_cast<uint16_t>((rowCount_ - 1) / 2), renderer.getScreenWidth() / 2);
  if (initial >= 0) cursorOrder_ = initial;
  requestUpdate();
}

void ClipSelectionActivity::extractWords() {
  words_.clear();
  visuals_.clear();
  readingOrder_.clear();
  words_.reserve(MAX_VISIBLE_WORDS);
  visuals_.reserve(MAX_VISIBLE_WORDS);
  readingOrder_.reserve(MAX_VISIBLE_WORDS);
  rowCount_ = 0;

  std::string pageText;
  pageText.reserve(MAX_VISIBLE_TEXT_BYTES);
  size_t visibleTextBytes = 0;
  uint8_t styleMask = 0;
  uint32_t visibleWordIndex = 0;
  bool reachedCap = false;

  for (const auto& element : page_->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;

    const int y = line->yPos + marginTop_;
    const bool lineVisible = y + lineHeight_ > 0 && y < renderer.getScreenHeight();

    std::vector<uint16_t> lineOrder;
    lineOrder.reserve(std::min<size_t>(block->wordCount(), MAX_VISIBLE_WORDS - words_.size()));
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const std::string_view text(block->wordText(i), block->wordTextLen(i));
      if (!ClippingPageTools::hasVisibleText(text)) continue;

      // pageWordIndex is based on every textual token in the serialized page,
      // including a geometrically clipped line. Highlight replay uses the same
      // definition, so hidden layout elements cannot shift persisted indices.
      if (visibleWordIndex > UINT16_MAX) {
        reachedCap = true;
        break;
      }
      const uint16_t pageWordIndex = static_cast<uint16_t>(visibleWordIndex++);
      if (!lineVisible) continue;
      if (text.size() > ClippingCodec::MAX_TEXT_BYTES || !ClippingCodec::isValidUtf8(text)) continue;
      if (words_.size() >= MAX_VISIBLE_WORDS) {
        reachedCap = true;
        break;
      }
      // TextBlock already owns these bytes in the resident Page. Bound the
      // aggregate duplicate held by Word::text and the font warm-up string;
      // a word-count cap alone still permits hundreds of kilobytes of URLs or
      // other long unbroken tokens on an ESP32.
      if (text.size() > MAX_VISIBLE_TEXT_BYTES - visibleTextBytes) {
        reachedCap = true;
        break;
      }

      ClipTextBuilder::Word word;
      word.x = line->xPos + block->wordXpos(i) + marginLeft_;
      word.y = y;
      word.height = lineHeight_;
      word.pageIndex = 0;
      word.pageWordIndex = pageWordIndex;
      word.text.assign(text);
      word.paragraphStart = startsParagraph(text);
      // Current TextBlock serialization does not expose layout-inserted
      // hyphen provenance. Keep authored text exact; the builder API is ready
      // to remove inserted hyphens when that signal becomes available.
      word.endsWithInsertedHyphen = false;

      const uint16_t wordIndex = static_cast<uint16_t>(words_.size());
      words_.push_back(std::move(word));
      visuals_.push_back({rowCount_, block->wordStyle(i)});
      lineOrder.push_back(wordIndex);

      pageText.append(text);
      pageText.push_back(' ');
      visibleTextBytes += text.size();
      const uint8_t style = static_cast<uint8_t>(block->wordStyle(i)) & 0x03U;
      styleMask |= static_cast<uint8_t>(1U << style);
    }

    if (!lineOrder.empty()) {
      const bool rtl = block->getBlockStyle().isRtl;
      std::stable_sort(lineOrder.begin(), lineOrder.end(), [this, rtl](const uint16_t lhs, const uint16_t rhs) {
        if (words_[lhs].x != words_[rhs].x) return rtl ? words_[lhs].x > words_[rhs].x : words_[lhs].x < words_[rhs].x;
        return words_[lhs].pageWordIndex < words_[rhs].pageWordIndex;
      });
      readingOrder_.insert(readingOrder_.end(), lineOrder.begin(), lineOrder.end());
      ++rowCount_;
    }
    if (reachedCap) break;
  }

  if (words_.empty()) return;
  if (styleMask == 0) styleMask = 0x01U;
  renderer.ensureSdCardFontReady(fontId_, pageText.c_str(), styleMask);
  for (size_t i = 0; i < words_.size(); ++i) {
    words_[i].width = renderer.getTextAdvanceX(fontId_, words_[i].text.c_str(), visuals_[i].style);
  }

  // Front-button hints move to a different physical edge as the reader rotates.
  // Keep persisted pageWordIndex values intact, but omit words hidden entirely
  // by that reserved strip from keyboard navigation.
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  std::vector<uint16_t> selectableOrder;
  selectableOrder.reserve(readingOrder_.size());
  uint16_t compactRow = 0;
  uint16_t previousRow = 0;
  bool haveRow = false;
  for (const uint16_t wordIndex : readingOrder_) {
    const auto& word = words_[wordIndex];
    const int right = word.x + std::max(1, word.width);
    const int bottom = word.y + std::max(1, word.height);
    const bool intersectsSafeArea =
        word.x < safe.x + safe.width && right > safe.x && word.y < safe.y + safe.height && bottom > safe.y;
    if (!intersectsSafeArea) continue;

    const uint16_t originalRow = visuals_[wordIndex].row;
    if (!haveRow || originalRow != previousRow) {
      if (haveRow) ++compactRow;
      previousRow = originalRow;
      haveRow = true;
    }
    visuals_[wordIndex].row = compactRow;
    selectableOrder.push_back(wordIndex);
  }
  readingOrder_.swap(selectableOrder);
  rowCount_ = haveRow ? static_cast<uint16_t>(compactRow + 1) : 0;
}

int ClipSelectionActivity::closestOrderInRow(const uint16_t row, const int centerX) const {
  int bestOrder = -1;
  int bestDistance = INT_MAX;
  for (size_t order = 0; order < readingOrder_.size(); ++order) {
    const uint16_t wordIndex = readingOrder_[order];
    if (visuals_[wordIndex].row != row) continue;
    const auto& word = words_[wordIndex];
    const int distance = std::abs(word.x + word.width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      bestOrder = static_cast<int>(order);
    }
  }
  return bestOrder;
}

bool ClipSelectionActivity::buildSelection(ClipTextBuilder::Result& result) const {
  if (anchorOrder_ < 0 || readingOrder_.empty()) return false;
  const size_t first = static_cast<size_t>(std::min(anchorOrder_, cursorOrder_));
  const size_t last = static_cast<size_t>(std::max(anchorOrder_, cursorOrder_));
  return ClipTextBuilder::build(words_, readingOrder_, first, last, sectionPage_, sectionPageCount_, result) ==
         ClipTextBuilder::Status::Ok;
}

bool ClipSelectionActivity::selectionBuildsAt(const int orderIndex) const {
  if (anchorOrder_ < 0) return true;
  const int first = std::min(anchorOrder_, orderIndex);
  const int last = std::max(anchorOrder_, orderIndex);

  // Raw token bytes plus one possible separator per boundary are an upper
  // bound on the normalized ClipTextBuilder output. This allocation-free
  // check keeps cursor movement responsive and guarantees the final build
  // cannot exceed the 512-byte storage limit. It is intentionally a little
  // conservative when punctuation or normalized whitespace removes bytes.
  size_t upperBound = static_cast<size_t>(last - first);
  for (int order = first; order <= last; ++order) {
    const size_t wordBytes = words_[readingOrder_[order]].text.size();
    if (upperBound > ClippingCodec::MAX_TEXT_BYTES || wordBytes > ClippingCodec::MAX_TEXT_BYTES - upperBound)
      return false;
    upperBound += wordBytes;
  }
  return upperBound <= ClippingCodec::MAX_TEXT_BYTES;
}

void ClipSelectionActivity::moveHorizontal(const int direction) {
  const int target = cursorOrder_ + direction;
  if (target < 0 || target >= static_cast<int>(readingOrder_.size()) || !selectionBuildsAt(target)) return;
  cursorOrder_ = target;
  requestUpdate();
}

void ClipSelectionActivity::moveVertical(const int direction) {
  const uint16_t currentWordIndex = readingOrder_[cursorOrder_];
  const auto& current = words_[currentWordIndex];
  const int targetRow = static_cast<int>(visuals_[currentWordIndex].row) + direction;
  if (targetRow < 0 || targetRow >= static_cast<int>(rowCount_)) return;

  const int target = closestOrderInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2);
  if (target < 0 || target == cursorOrder_ || !selectionBuildsAt(target)) return;
  cursorOrder_ = target;
  requestUpdate();
}

void ClipSelectionActivity::confirmSelection() {
  if (readingOrder_.empty()) return;
  if (anchorOrder_ < 0) {
    // A validated one-word selection guarantees that the second Confirm can
    // always return a result; movement is constrained to ranges <= 512 bytes.
    anchorOrder_ = cursorOrder_;
    ClipTextBuilder::Result ignored;
    if (!buildSelection(ignored)) {
      anchorOrder_ = -1;
      return;
    }
    requestUpdate();
    return;
  }

  ClipTextBuilder::Result built;
  if (!buildSelection(built)) return;

  ClippingSelectionResult selection;
  selection.text = std::move(built.text);
  selection.startWordIndex = built.startWordIndex;
  selection.endWordIndex = built.endWordIndex;
  selection.startPage = built.startPage;
  selection.endPage = built.endPage;
  selection.pageCount = built.pageCount;
  selection.startPageWordIndex = built.startPageWordIndex;
  selection.endPageWordIndex = built.endPageWordIndex;
  selection.paragraphIndex = paragraphIndex_;
  selection.wordCount = built.wordCount;
  selection.pageFingerprint = pageFingerprint_;
  setResult(std::move(selection));
  finish();
}

void ClipSelectionActivity::cancel() {
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  setResult(std::move(cancelled));
  finish();
}

void ClipSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmPressSeen_ = true;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const bool accept = confirmPressSeen_;
    confirmPressSeen_ = false;
    if (accept) confirmSelection();
    return;
  }

  if (readingOrder_.empty()) return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    moveHorizontal(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    moveHorizontal(1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    moveVertical(1);
  }
}

void ClipSelectionActivity::drawSelection() const {
  if (readingOrder_.empty()) return;
  const int first = anchorOrder_ < 0 ? cursorOrder_ : std::min(anchorOrder_, cursorOrder_);
  const int last = anchorOrder_ < 0 ? cursorOrder_ : std::max(anchorOrder_, cursorOrder_);
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  if (anchorOrder_ >= 0) {
    for (int order = first; order <= last; ++order) {
      const auto& word = words_[readingOrder_[order]];
      const int left = std::max(0, word.x);
      const int right = std::min(screenWidth, word.x + std::max(1, word.width));
      const int underlineY = std::clamp(word.y + word.height - 2, 0, screenHeight - 1);
      if (right <= left) continue;
      renderer.drawLine(left, underlineY, right - 1, underlineY, 2, true);
      for (int x = left + ((left + underlineY) & 1); x < right; x += 2) {
        if (underlineY > 1) renderer.drawPixel(x, underlineY - 2, true);
      }
    }
  }

  const auto& cursor = words_[readingOrder_[cursorOrder_]];
  const int left = std::max(0, cursor.x - 2);
  const int top = std::max(0, cursor.y - 2);
  const int right = std::min(screenWidth, cursor.x + std::max(1, cursor.width) + 2);
  const int bottom = std::min(screenHeight, cursor.y + cursor.height + 2);
  if (right - left >= 2 && bottom - top >= 2) {
    renderer.drawRect(left, top, right - left, bottom - top, 2, true);
  }
}

void ClipSelectionActivity::drawHints() const {
  const char* confirm = anchorOrder_ < 0 ? tr(STR_SELECT) : tr(STR_DONE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ClipSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (!page_) {
    renderer.displayBuffer();
    return;
  }

  // Always rebuild the complete framebuffer. This costs more work per cursor
  // move than a snapshot, but avoids a large persistent allocation and stale
  // framebuffer restores when page content or orientation changes.
  auto* fontCache = renderer.getFontCacheManager();
  auto scope = fontCache->createPrewarmScope();
  page_->render(renderer, fontId_, marginLeft_, marginTop_);
  scope.endScanAndPrewarm();
  page_->render(renderer, fontId_, marginLeft_, marginTop_);

  drawSelection();
  drawHints();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
