#include "CoverLoader.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Xtc.h>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "util/StringUtils.h"

namespace {
class Lock {
  SemaphoreHandle_t sem;

 public:
  explicit Lock(SemaphoreHandle_t sem) : sem(sem) { xSemaphoreTake(sem, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(sem); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};
}  // namespace

// Called from the main task (HomeActivity)
void CoverLoader::start(const std::vector<RecentBook>* recents, int height, Activity* activity) {
  // No locking here prior to starting the CoverLoader task
  books = recents;
  owner = activity;
  coverHeight = height;
  complete = false;
  state = State::Idle;
  lastMerged = 0;
  processed = 0;
  results.assign(books->size(), CoverResult::Skipped);

  bool needsGeneration = false;
  for (const RecentBook& book : *books) {
    if (!book.coverBmpPath.empty()) {
      std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(thumbPath.c_str())) {
        needsGeneration = true;
        break;
      }
    }
  }

  if (!needsGeneration) {
    processed = static_cast<int>(books->size());
    lastMerged = static_cast<int>(books->size());
    complete = true;
    return;
  }

  state = State::Running;
  xTaskCreate(&taskTrampoline, "CoverLoader", 8192, this, 0, &taskHandle);
}

// Called from the main task (HomeActivity)
void CoverLoader::stop() {
  {
    Lock lock(mutex);
    if (state != State::Running) {
      taskHandle = nullptr;
      return;
    }
    callerTask = xTaskGetCurrentTaskHandle();
    state = State::Stopping;
  }
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  taskHandle = nullptr;
}

// Called from the render task
CoverLoader::MergeStatus CoverLoader::mergeResults(std::vector<RecentBook>& mutableBooks) {
  if (complete && lastMerged >= static_cast<int>(mutableBooks.size())) {
    return {false, true};
  }

  int ready;
  {
    Lock lock(mutex);
    ready = processed;
  }

  bool changed = false;
  for (int i = lastMerged; i < ready; i++) {
    CoverResult result = results[i];
    if (result == CoverResult::Failed) {
      RecentBook& book = mutableBooks[i];
      RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
      book.coverBmpPath = "";
      changed = true;
    } else if (result == CoverResult::Generated) {
      changed = true;
    }
  }
  lastMerged = ready;

  complete = ready == static_cast<int>(mutableBooks.size());
  return {changed, complete};
}

void CoverLoader::taskTrampoline(void* param) { static_cast<CoverLoader*>(param)->taskLoop(); }

// Runs on the CoverLoader task
void CoverLoader::taskLoop() {
  for (size_t i = 0; i < books->size(); i++) {
    {
      Lock lock(mutex);
      if (state == State::Stopping) break;
    }

    const RecentBook& book = books->at(i);
    CoverResult result = CoverResult::Skipped;

    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        result = CoverResult::Failed;
        if (StringUtils::checkFileExtension(book.path, ".epub")) {
          Epub epub(book.path, "/.crosspoint");
          epub.load(false, true);
          {
            Lock lock(mutex);
            if (state == State::Stopping) break;
          }
          result = epub.generateThumbBmp(coverHeight) ? CoverResult::Generated : CoverResult::Failed;
        } else if (StringUtils::checkFileExtension(book.path, ".xtch") ||
                   StringUtils::checkFileExtension(book.path, ".xtc")) {
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            {
              Lock lock(mutex);
              if (state == State::Stopping) break;
            }
            result = xtc.generateThumbBmp(coverHeight) ? CoverResult::Generated : CoverResult::Failed;
          }
        }
      }
    }

    {
      Lock lock(mutex);
      results[i] = result;
      processed = static_cast<int>(i) + 1;
    }

    if (result != CoverResult::Skipped) {
      owner->requestUpdate();
    }
  }

  TaskHandle_t caller;
  {
    Lock lock(mutex);
    state = State::Idle;
    caller = callerTask;
  }
  if (caller) {
    xTaskNotifyGive(caller);
  }
  vTaskDelete(nullptr);
}
