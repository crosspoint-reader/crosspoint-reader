#include "ClippingListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

size_t nextUtf8Boundary(const std::string_view text, size_t offset) {
  if (offset >= text.size()) return text.size();
  ++offset;
  while (offset < text.size() && (static_cast<uint8_t>(text[offset]) & 0xC0U) == 0x80U) ++offset;
  return offset;
}

}  // namespace

void ClippingListActivity::onEnter() {
  Activity::onEnter();
  RenderLock lock(*this);
  if (store_.size() == 0) {
    selectedIndex_ = 0;
  } else {
    selectedIndex_ = std::clamp(selectedIndex_, 0, static_cast<int>(store_.size()) - 1);
  }
  requestUpdate();
}

void ClippingListActivity::cancelActivity() {
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  setResult(std::move(cancelled));
  finish();
}

void ClippingListActivity::handleBack() {
  switch (view_) {
    case View::DeletePrompt:
    case View::DeleteConfirm:
      view_ = deleteReturnView_;
      requestUpdate();
      return;
    case View::Detail:
      view_ = View::List;
      requestUpdate();
      return;
    case View::List:
      cancelActivity();
      return;
  }
}

void ClippingListActivity::beginDelete() {
  if (store_.size() == 0 || selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(store_.size())) return;
  deleteReturnView_ = view_;
  view_ = View::DeletePrompt;
  deleteFailed_ = false;
  longPressHandled_ = true;
  ignoreConfirmRelease_ = true;
  confirmPressSeen_ = false;
  requestUpdate();
}

void ClippingListActivity::deleteSelected() {
  if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(store_.size())) {
    view_ = View::List;
    requestUpdate();
    return;
  }

  if (!store_.remove(static_cast<size_t>(selectedIndex_))) {
    deleteFailed_ = true;
    view_ = deleteReturnView_;
    requestUpdate();
    return;
  }

  detailText_.clear();
  detailLines_.clear();
  detailPage_ = 0;
  detailPageCount_ = 1;
  if (store_.size() == 0) {
    selectedIndex_ = 0;
  } else if (selectedIndex_ >= static_cast<int>(store_.size())) {
    selectedIndex_ = static_cast<int>(store_.size()) - 1;
  }
  view_ = View::List;
  deleteFailed_ = false;
  requestUpdate();
}

void ClippingListActivity::openDetail() {
  if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(store_.size())) return;

  detailText_.clear();
  if (!store_.readText(static_cast<size_t>(selectedIndex_), detailText_)) {
    detailText_ = tr(STR_ERROR_GENERAL_FAILURE);
  }
  wrapDetail();
  view_ = View::Detail;
  deleteFailed_ = false;
  requestUpdate();
}

void ClippingListActivity::finishWithJump() {
  const auto* clipping = store_.at(static_cast<size_t>(selectedIndex_));
  if (!clipping) return;

  std::string text;
  if (!store_.readText(static_cast<size_t>(selectedIndex_), text)) {
    deleteFailed_ = true;
    requestUpdate();
    return;
  }

  ClippingJumpResult jump;
  jump.bookTitle = store_.book().title;
  jump.bookAuthor = store_.book().author;
  jump.bookPath = store_.book().path;
  jump.bookType = store_.book().bookType;
  jump.storePath = store_.path();
  jump.chapterTitle = clipping->chapterTitle;
  jump.storeFileLength = store_.fileLength();
  jump.storeFormat = static_cast<uint8_t>(store_.format());
  jump.clippingIndex = static_cast<uint16_t>(selectedIndex_);
  jump.spineIndex = clipping->spineIndex;
  jump.startPage = clipping->startPage;
  jump.endPage = clipping->endPage;
  jump.pageCount = clipping->pageCount;
  jump.startWordIndex = clipping->startWordIndex;
  jump.endWordIndex = clipping->endWordIndex;
  jump.wordCount = clipping->wordCount;
  jump.paragraphIndex = clipping->paragraphIndex;
  jump.timestamp = clipping->timestamp;
  jump.textOffset = clipping->textOffset;
  jump.textLength = clipping->textLength;
  jump.textCrc32 = ClippingCodec::crc32(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  jump.pageFingerprint = clipping->pageFingerprint;
  setResult(std::move(jump));
  finish();
}

void ClippingListActivity::handleConfirmRelease() {
  switch (view_) {
    case View::List:
      openDetail();
      return;
    case View::Detail:
      finishWithJump();
      return;
    case View::DeletePrompt:
      view_ = View::DeleteConfirm;
      requestUpdate();
      return;
    case View::DeleteConfirm:
      deleteSelected();
      return;
  }
}

void ClippingListActivity::moveList(const int direction) {
  const int total = static_cast<int>(store_.size());
  if (total <= 0) return;
  selectedIndex_ = direction > 0 ? ButtonNavigator::nextIndex(selectedIndex_, total)
                                 : ButtonNavigator::previousIndex(selectedIndex_, total);
  deleteFailed_ = false;
  requestUpdate();
}

void ClippingListActivity::moveListPage(const int direction) {
  const int total = static_cast<int>(store_.size());
  if (total <= 0) return;
  const int pageItems = std::max(1, GUI.getListPageItems(listArea().height, true));
  selectedIndex_ = direction > 0 ? ButtonNavigator::nextPageIndex(selectedIndex_, total, pageItems)
                                 : ButtonNavigator::previousPageIndex(selectedIndex_, total, pageItems);
  deleteFailed_ = false;
  requestUpdate();
}

void ClippingListActivity::moveDetailPage(const int direction) {
  const int next = std::clamp(detailPage_ + direction, 0, detailPageCount_ - 1);
  if (next == detailPage_) return;
  detailPage_ = next;
  requestUpdate();
}

void ClippingListActivity::loop() {
  // render() runs on a separate task. Keep the store and every piece of UI
  // state it observes under the same lock for the complete input operation,
  // including lazy SD reads and transactional deletes.
  RenderLock lock(*this);

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmPressSeen_ = true;
    longPressHandled_ = false;
    deleteFailed_ = false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    handleBack();
    return;
  }

  if ((view_ == View::List || view_ == View::Detail) && confirmPressSeen_ && !longPressHandled_ && store_.size() > 0 &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= DELETE_HOLD_MS) {
    beginDelete();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreConfirmRelease_) {
      ignoreConfirmRelease_ = false;
      longPressHandled_ = false;
      return;
    }
    const bool accept = confirmPressSeen_;
    confirmPressSeen_ = false;
    longPressHandled_ = false;
    if (accept) handleConfirmRelease();
    return;
  }

  if (view_ == View::List) {
    navigator_.onNextRelease([this] { moveList(1); });
    navigator_.onPreviousRelease([this] { moveList(-1); });
    navigator_.onNextContinuous([this] { moveListPage(1); });
    navigator_.onPreviousContinuous([this] { moveListPage(-1); });
  } else if (view_ == View::Detail) {
    navigator_.onNext([this] { moveDetailPage(1); });
    navigator_.onPrevious([this] { moveDetailPage(-1); });
  }
}

Rect ClippingListActivity::safeArea() const { return UITheme::getInstance().getScreenSafeArea(renderer, true, false); }

Rect ClippingListActivity::listArea() const {
  const Rect screen = safeArea();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int helpHeight = metrics.listRowHeight;
  const int bottom = screen.y + screen.height - helpHeight - metrics.verticalSpacing;
  return Rect{screen.x, top, screen.width, std::max(0, bottom - top)};
}

std::string ClippingListActivity::headerTitle() const {
  return store_.book().title.empty() ? std::string(tr(STR_BOOK)) : store_.book().title;
}

std::string ClippingListActivity::clippingTitle(const int index) const {
  const auto* clipping = index < 0 ? nullptr : store_.at(static_cast<size_t>(index));
  if (!clipping) return {};
  return clipping->chapterTitle.empty() ? std::string(tr(STR_UNNAMED)) : clipping->chapterTitle;
}

std::string ClippingListActivity::clippingSubtitle(const int index) const {
  const auto* clipping = index < 0 ? nullptr : store_.at(static_cast<size_t>(index));
  if (!clipping) return {};

  std::string subtitle = std::string(tr(STR_SECTION_PREFIX)) + std::to_string(clipping->spineIndex + 1) + " - ";
  subtitle += std::to_string(clipping->startPage + 1);
  if (clipping->endPage != clipping->startPage) {
    subtitle += "-" + std::to_string(clipping->endPage + 1);
  }
  subtitle += "/" + std::to_string(clipping->pageCount);
  return subtitle;
}

void ClippingListActivity::wrapDetail() {
  detailLines_.clear();
  detailPage_ = 0;

  const Rect screen = safeArea();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int maxWidth = std::max(1, screen.width - metrics.contentSidePadding * 2);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int availableHeight = std::max(1, screen.y + screen.height - contentTop - metrics.verticalSpacing);
  const int lineHeight = std::max(1, renderer.getLineHeight(UI_10_FONT_ID));
  detailLinesPerPage_ = std::max(1, availableHeight / lineHeight);

  std::string current;
  const auto flushCurrent = [this, &current](const bool keepBlank) {
    if (!current.empty() || keepBlank) detailLines_.push_back(std::move(current));
    current.clear();
  };

  const auto appendToken = [this, &current, &flushCurrent, maxWidth](const std::string_view token) {
    if (token.empty()) return;
    std::string candidate = current.empty() ? std::string(token) : current + " " + std::string(token);
    if (renderer.getTextWidth(UI_10_FONT_ID, candidate.c_str()) <= maxWidth) {
      current = std::move(candidate);
      return;
    }
    if (!current.empty()) flushCurrent(false);

    size_t start = 0;
    while (start < token.size()) {
      size_t best = start;
      size_t next = nextUtf8Boundary(token, start);
      while (next <= token.size()) {
        const std::string part(token.substr(start, next - start));
        if (renderer.getTextWidth(UI_10_FONT_ID, part.c_str()) > maxWidth) break;
        best = next;
        if (next == token.size()) break;
        next = nextUtf8Boundary(token, next);
      }
      if (best == start) best = nextUtf8Boundary(token, start);  // one over-wide codepoint still makes progress
      current.assign(token.data() + start, best - start);
      start = best;
      if (start < token.size()) flushCurrent(false);
    }
  };

  size_t offset = 0;
  while (offset < detailText_.size()) {
    const char c = detailText_[offset];
    if (c == '\n') {
      flushCurrent(true);
      ++offset;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      ++offset;
      continue;
    }
    const size_t tokenStart = offset;
    while (offset < detailText_.size() && detailText_[offset] != ' ' && detailText_[offset] != '\t' &&
           detailText_[offset] != '\r' && detailText_[offset] != '\n') {
      ++offset;
    }
    appendToken(std::string_view(detailText_).substr(tokenStart, offset - tokenStart));
  }
  flushCurrent(false);
  while (detailLines_.size() > 1 && detailLines_.back().empty()) detailLines_.pop_back();
  if (detailLines_.empty()) detailLines_.emplace_back();

  detailPageCount_ =
      std::max(1, (static_cast<int>(detailLines_.size()) + detailLinesPerPage_ - 1) / detailLinesPerPage_);
}

void ClippingListActivity::renderList(const Rect screen) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const std::string title = headerTitle();
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());

  const Rect list = listArea();
  if (store_.size() == 0) {
    GUI.drawHelpText(renderer, Rect{screen.x, list.y + list.height / 2, screen.width, metrics.listRowHeight},
                     tr(STR_NO_ENTRIES));
    return;
  }

  GUI.drawList(
      renderer, list, static_cast<int>(store_.size()), selectedIndex_,
      [this](const int index) { return clippingTitle(index); },
      [this](const int index) { return clippingSubtitle(index); });

  const int helpY = list.y + list.height + metrics.verticalSpacing;
  GUI.drawHelpText(renderer, Rect{screen.x, helpY, screen.width, metrics.listRowHeight},
                   deleteFailed_ ? tr(STR_ERROR_GENERAL_FAILURE) : tr(STR_HOLD_OPEN_TO_DELETE));
}

void ClippingListActivity::renderDetail(const Rect screen) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto* clipping = store_.at(static_cast<size_t>(selectedIndex_));
  const std::string title = clipping ? clippingTitle(selectedIndex_) : headerTitle();
  std::string subtitle = clipping ? clippingSubtitle(selectedIndex_) : std::string();
  if (detailPageCount_ > 1) {
    subtitle += "  " + std::to_string(detailPage_ + 1) + "/" + std::to_string(detailPageCount_);
  }
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str(), subtitle.empty() ? nullptr : subtitle.c_str());

  const int x = screen.x + metrics.contentSidePadding;
  const int top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineHeight = std::max(1, renderer.getLineHeight(UI_10_FONT_ID));
  const int first = detailPage_ * detailLinesPerPage_;
  const int last = std::min(first + detailLinesPerPage_, static_cast<int>(detailLines_.size()));
  for (int index = first; index < last; ++index) {
    if (!detailLines_[index].empty()) {
      renderer.drawText(UI_10_FONT_ID, x, top + (index - first) * lineHeight, detailLines_[index].c_str());
    }
  }

  if (deleteFailed_) {
    GUI.drawHelpText(
        renderer, Rect{screen.x, screen.y + screen.height - metrics.listRowHeight, screen.width, metrics.listRowHeight},
        tr(STR_ERROR_GENERAL_FAILURE));
  }
}

void ClippingListActivity::renderDelete(const Rect screen) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const std::string title = headerTitle();
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());

  const int rowHeight = metrics.listWithSubtitleRowHeight;
  const int rowY = screen.y + (screen.height - rowHeight) / 2;
  const char* prompt = view_ == View::DeletePrompt ? tr(STR_CONFIRM) : tr(STR_DELETE);
  GUI.drawHelpText(renderer, Rect{screen.x, rowY - metrics.headerHeight, screen.width, metrics.listRowHeight}, prompt);
  GUI.drawList(
      renderer, Rect{screen.x, rowY, screen.width, rowHeight}, 1, 0,
      [this](const int) { return clippingTitle(selectedIndex_); },
      [this](const int) { return clippingSubtitle(selectedIndex_); });
}

void ClippingListActivity::drawHints() const {
  const char* back = tr(STR_BACK);
  const char* confirm = "";
  const char* previous = "";
  const char* next = "";
  switch (view_) {
    case View::List:
      confirm = store_.size() == 0 ? "" : tr(STR_OPEN);
      previous = store_.size() == 0 ? "" : tr(STR_DIR_UP);
      next = store_.size() == 0 ? "" : tr(STR_DIR_DOWN);
      break;
    case View::Detail:
      confirm = tr(STR_OPEN);
      previous = detailPage_ > 0 ? tr(STR_DIR_UP) : "";
      next = detailPage_ + 1 < detailPageCount_ ? tr(STR_DIR_DOWN) : "";
      break;
    case View::DeletePrompt:
      back = tr(STR_CANCEL);
      confirm = tr(STR_CONFIRM);
      break;
    case View::DeleteConfirm:
      back = tr(STR_CANCEL);
      confirm = tr(STR_DELETE);
      break;
  }
  const auto labels = mappedInput.mapLabels(back, confirm, previous, next);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ClippingListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect screen = safeArea();
  switch (view_) {
    case View::List:
      renderList(screen);
      break;
    case View::Detail:
      renderDetail(screen);
      break;
    case View::DeletePrompt:
    case View::DeleteConfirm:
      renderDelete(screen);
      break;
  }
  drawHints();
  renderer.displayBuffer();
}
