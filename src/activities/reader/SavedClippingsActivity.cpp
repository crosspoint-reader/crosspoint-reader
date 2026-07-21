#include "SavedClippingsActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <memory>
#include <utility>

#include "ClippingListActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

void SavedClippingsActivity::onEnter() {
  Activity::onEnter();
  RenderLock lock(*this);
  reloadCatalog(true);
  requestUpdate();
}

void SavedClippingsActivity::onExit() {
  openedStore_.unload();
  catalog_ = {};
  Activity::onExit();
}

void SavedClippingsActivity::onResume() {
  // The child can delete the last clipping for a book or migrate a legacy
  // store. Re-scan only here, while returning to this screen; Home never pays
  // this SD I/O cost.
  RenderLock lock(*this);
  openedStore_.unload();
  reloadCatalog(false);
  requestUpdate();
}

void SavedClippingsActivity::reloadCatalog(const bool clearNotice) {
  std::string selectedBookPath;
  if (selectedIndex_ > 0 && selectedIndex_ <= static_cast<int>(catalog_.entries.size())) {
    selectedBookPath = catalog_.entries[static_cast<size_t>(selectedIndex_ - 1)].book.path;
  }

  if (clearNotice) notice_.clear();
  catalogLoadResult_ = ClippingStore::loadCatalog(catalog_);
  SavedClippingsModel::sortEntries(catalog_.entries);

  if (!selectedBookPath.empty()) {
    const auto selected = std::find_if(catalog_.entries.begin(), catalog_.entries.end(),
                                       [&](const auto& entry) { return entry.book.path == selectedBookPath; });
    if (selected != catalog_.entries.end()) {
      selectedIndex_ = static_cast<int>(std::distance(catalog_.entries.begin(), selected)) + 1;
      return;
    }
  }
  selectedIndex_ = std::clamp(selectedIndex_, 0, rowCount() - 1);
}

void SavedClippingsActivity::handleClippingResult(const ActivityResult& result) {
  if (result.isCancelled) return;

  RenderLock lock(*this);
  const auto* jump = std::get_if<ClippingJumpResult>(&result.data);
  if (!jump || jump->bookPath != openedStore_.book().path || jump->bookType != openedStore_.book().bookType ||
      jump->storePath != openedStore_.path()) {
    notice_ = tr(STR_CLIPPING_JUMP_UNAVAILABLE);
    return;
  }
  HalFile book = Storage.open(jump->bookPath.c_str());
  const bool regularBook = book && !book.isDirectory();
  const bool bookClosed = !book || book.close();
  if (!regularBook || !bookClosed) {
    // The clipping remains viewable/exportable, but there is no book to open.
    // Never fall through to ReaderActivity, which would otherwise leave this
    // screen without being able to validate a target.
    notice_ = tr(STR_BOOK_FILE_MISSING);
    return;
  }
  if (jump->bookType != "epub" || !FsHelpers::hasEpubExtension(jump->bookPath)) {
    notice_ = tr(STR_CLIPPING_JUMP_UNAVAILABLE);
    return;
  }

  const std::string bookPath = jump->bookPath;
  ClippingJumpResult request = *jump;
  lock.unlock();
  activityManager.goToReader(bookPath, std::move(request));
}

void SavedClippingsActivity::openSelectedBook() {
  if (selectedIndex_ <= 0 || selectedIndex_ > static_cast<int>(catalog_.entries.size())) return;
  const auto& selected = catalog_.entries[static_cast<size_t>(selectedIndex_ - 1)];
  openedStore_.unload();
  const ClippingStore::LoadResult load =
      openedStore_.loadForBook(selected.book.path, selected.book.title, selected.book.author, selected.book.bookType);
  if (!openedStore_.isLoaded() || openedStore_.size() == 0 || load == ClippingStore::LoadResult::Ready) {
    openedStore_.unload();
    notice_ = tr(STR_OPEN_SAVED_ITEMS_FAILED);
    requestUpdate();
    return;
  }

  startActivityForResult(std::make_unique<ClippingListActivity>(renderer, mappedInput, openedStore_),
                         [this](const ActivityResult& result) { handleClippingResult(result); });
}

void SavedClippingsActivity::exportAll() {
  {
    RenderLock lock(*this);
    if (!exportAvailable()) {
      notice_.clear();
      switch (SavedClippingsModel::state(catalogLoadResult_, catalog_)) {
        case SavedClippingsModel::CatalogState::Empty:
          notice_ = tr(STR_SAVED_ITEMS_EMPTY);
          break;
        case SavedClippingsModel::CatalogState::Incomplete:
          notice_ = tr(STR_SAVED_ITEMS_INCOMPLETE);
          break;
        case SavedClippingsModel::CatalogState::ReadError:
          notice_ = tr(STR_SAVED_ITEMS_READ_FAILED);
          break;
        case SavedClippingsModel::CatalogState::Ready:
          break;
      }
      requestUpdate();
      return;
    }
    notice_ = tr(STR_EXPORTING_CLIPPINGS);
  }

  // Give e-ink users explicit feedback before the bounded SD read begins.
  requestUpdateAndWait();

  std::string outputPath;
  const ClippingStore::ExportResult result = ClippingStore::exportCatalog(catalog_, outputPath);

  RenderLock lock(*this);
  switch (result) {
    case ClippingStore::ExportResult::Exported: {
      char message[128]{};
      std::snprintf(message, sizeof(message), tr(STR_EXPORT_SAVED_TO_FORMAT), outputPath.c_str());
      notice_ = message;
      break;
    }
    case ClippingStore::ExportResult::Empty:
      notice_ = tr(STR_SAVED_ITEMS_EMPTY);
      break;
    case ClippingStore::ExportResult::CatalogIncomplete:
      notice_ = tr(STR_SAVED_ITEMS_INCOMPLETE);
      break;
    case ClippingStore::ExportResult::SourceChanged:
      notice_ = tr(STR_SAVED_ITEMS_CHANGED);
      reloadCatalog(false);
      break;
    case ClippingStore::ExportResult::NoAvailableName:
    case ClippingStore::ExportResult::IoError:
      notice_ = tr(STR_EXPORT_CLIPPINGS_FAILED);
      break;
  }
  requestUpdate();
}

void SavedClippingsActivity::loop() {
  RenderLock lock(*this);

  navigator_.onNext([this] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, rowCount());
    notice_.clear();
    requestUpdate();
  });
  navigator_.onPrevious([this] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, rowCount());
    notice_.clear();
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lock.unlock();
    onGoHome(HomeMenuItem::SAVED_ITEMS);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex_ == 0) {
      lock.unlock();
      exportAll();
    } else {
      openSelectedBook();
    }
  }
}

std::string SavedClippingsActivity::rowTitle(const int index) const {
  if (index == 0) return tr(STR_EXPORT_ALL_CLIPPINGS);
  if (index < 0 || index > static_cast<int>(catalog_.entries.size())) return {};
  const auto& book = catalog_.entries[static_cast<size_t>(index - 1)].book;
  return book.title.empty() ? std::string(tr(STR_UNNAMED)) : book.title;
}

std::string SavedClippingsActivity::rowSubtitle(const int index) const {
  if (index == 0) return tr(STR_EXPORT_CLIPPINGS_DESC);
  if (index < 0 || index > static_cast<int>(catalog_.entries.size())) return {};
  const auto& entry = catalog_.entries[static_cast<size_t>(index - 1)];
  std::string subtitle = entry.book.author;
  if (!entry.bookExists) {
    if (!subtitle.empty()) subtitle += " - ";
    subtitle += tr(STR_BOOK_FILE_MISSING);
  }
  return subtitle;
}

std::string SavedClippingsActivity::rowValue(const int index) const {
  if (index <= 0 || index > static_cast<int>(catalog_.entries.size())) return {};
  const unsigned count = catalog_.entries[static_cast<size_t>(index - 1)].clippingCount;
  if (count == 1) return tr(STR_CLIPPING_COUNT_ONE);
  char value[32]{};
  std::snprintf(value, sizeof(value), tr(STR_CLIPPING_COUNT_FORMAT), count);
  return value;
}

UIIcon SavedClippingsActivity::rowIcon(const int index) const { return index == 0 ? Transfer : Bookmark; }

std::string SavedClippingsActivity::statusText() const {
  if (!notice_.empty()) return notice_;
  switch (SavedClippingsModel::state(catalogLoadResult_, catalog_)) {
    case SavedClippingsModel::CatalogState::Ready:
      return tr(STR_SAVED_ITEMS_HELP);
    case SavedClippingsModel::CatalogState::Empty:
      return tr(STR_SAVED_ITEMS_EMPTY);
    case SavedClippingsModel::CatalogState::Incomplete:
      return tr(STR_SAVED_ITEMS_INCOMPLETE);
    case SavedClippingsModel::CatalogState::ReadError:
      return tr(STR_SAVED_ITEMS_READ_FAILED);
  }
  return {};
}

void SavedClippingsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = screen.y + metrics.topPadding;
  const int contentTop = headerTop + metrics.headerHeight + metrics.verticalSpacing;
  const int helpHeight = metrics.listRowHeight;
  const int contentBottom = screen.y + screen.height - helpHeight - metrics.verticalSpacing;

  GUI.drawHeader(renderer, Rect{screen.x, headerTop, screen.width, metrics.headerHeight}, tr(STR_SAVED_ITEMS));
  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, std::max(0, contentBottom - contentTop)}, rowCount(),
      selectedIndex_, [this](const int index) { return rowTitle(index); },
      [this](const int index) { return rowSubtitle(index); }, [this](const int index) { return rowIcon(index); },
      [this](const int index) { return rowValue(index); }, false,
      [this](const int index) { return index == 0 && !exportAvailable(); });

  const std::string status = statusText();
  GUI.drawHelpText(renderer, Rect{screen.x, contentBottom + metrics.verticalSpacing, screen.width, helpHeight},
                   status.c_str());

  const char* confirm = selectedIndex_ == 0 ? tr(STR_EXPORT) : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
