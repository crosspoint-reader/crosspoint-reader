#include "ImageCacheService.h"

#include <Arduino.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include <utility>

#include "Page.h"
#include "blocks/ImageBlock.h"

ImageCacheService& ImageCacheService::getInstance() {
  static ImageCacheService instance;
  return instance;
}

void ImageCacheService::taskTrampoline(void* param) { static_cast<ImageCacheService*>(param)->run(); }

void ImageCacheService::start() {
  if (task_) return;
  if (!mutex_) {
    mutex_ = xSemaphoreCreateMutex();
    completion_ = xSemaphoreCreateBinary();
    stopped_ = xSemaphoreCreateBinary();
    assert(mutex_ && completion_ && stopped_);
  }
  stopRequested_.store(false);
  completedSerial_.store(0);
  paused_ = false;
  // Priority 1: the same tier as the main loop and render tasks. Time slicing
  // keeps input responsive; the decode yields every 250ms inside the codec. The
  // stack matches the render task's non-vector size: the codec work runs here.
  xTaskCreate(&taskTrampoline, "ImageCache", 8192, this, 1, &task_);
  assert(task_ && "Failed to create image cache task");
}

void ImageCacheService::stop() {
  if (!task_) return;
  stopRequested_.store(true);
  // The worker blocks on ulTaskNotifyTake(), so it must be woken with a task
  // notification (not a semaphore).
  xTaskNotifyGive(task_);
  // The worker only ever finishes the one job in flight, so this is bounded by
  // a single decode. Bound it anyway: a wedged worker must not be able to lock
  // the whole UI forever. On timeout we proceed (the queued jobs are dropped
  // and nothing referenced by a live waiter is touched) and log loudly.
  constexpr uint32_t STOP_TIMEOUT_MS = 8000;
  if (xSemaphoreTake(stopped_, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) {
    LOG_ERR("ICS", "Worker did not stop within %lums; continuing", (unsigned long)STOP_TIMEOUT_MS);
  }
  task_ = nullptr;

  // Any foreground waiter whose job is being dropped must wake now, not after
  // its timeout: the worker is gone and will never advance the serial.
  completedSerial_.store(nextSerial_ - 1);
  xSemaphoreGive(completion_);

  xSemaphoreTake(mutex_, portMAX_DELAY);
  for (Job& job : jobs_) {
    job = Job{};
  }
  xSemaphoreGive(mutex_);
}

void ImageCacheService::pause() {
  if (!mutex_) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  paused_ = true;
  xSemaphoreGive(mutex_);
}

void ImageCacheService::resume() {
  if (!mutex_) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  paused_ = false;
  xSemaphoreGive(mutex_);
  if (task_) xTaskNotifyGive(task_);
}

void ImageCacheService::run() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (stopRequested_.load()) break;

    Job job;
    bool haveJob = false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (jobs_[0].valid) {
      job = std::move(jobs_[0]);
      // Clear the slot: a moved-from Job still holds its raw borrowed pointers
      // and valid flag, so leaving it would make the worker re-run the job
      // against a Page the waiter has already freed.
      jobs_[0] = Job{};
      haveJob = true;
    } else if (!paused_ && jobs_[1].valid) {
      job = std::move(jobs_[1]);
      jobs_[1] = Job{};
      haveJob = true;
    }
    xSemaphoreGive(mutex_);
    if (!haveJob) continue;

    // The decode starts after the idle low-power drop; without the lock a ~1.5s
    // JPEG would stretch to ~10s at 10 MHz.
    HalPowerManager::Lock powerLock;
    const auto t0 = millis();
    const bool ok = runJob(job);
    LOG_DBG("ICS", "%s %s decode in %lums", job.foreground ? "Foreground" : "Background", ok ? "image" : "failed image",
            millis() - t0);

    completedSerial_.store(job.serial);
    xSemaphoreGive(completion_);
  }
  xSemaphoreGive(stopped_);
  vTaskDelete(nullptr);
}

bool ImageCacheService::runJob(Job& job) {
  if (job.block) {
    if (job.block->hasValidCache()) return true;
    return job.block->prefetch(job.target, job.x, job.y);
  }

  const Page* page = job.ownedPage ? job.ownedPage.get() : job.borrowedPage;
  if (!page) return false;
  while (page->hasImagesNeedingDecode()) {
    if (!page->prefetchOneImage(job.target, job.x, job.y)) break;
  }
  return !page->hasImagesNeedingDecode();
}

uint32_t ImageCacheService::postForeground(Job&& job) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  job.valid = true;
  job.foreground = true;
  job.serial = nextSerial_++;
  job.ownedPage.reset();
  jobs_[0] = std::move(job);
  const uint32_t serial = jobs_[0].serial;
  xSemaphoreGive(mutex_);
  if (task_) xTaskNotifyGive(task_);
  return serial;
}

void ImageCacheService::waitForCompletion(const uint32_t serial) {
  // Unbounded: the job borrows the caller's Page/ImageBlock, so the caller must
  // not return (and free it) until the worker is done with it. stop() advances
  // completedSerial_ and signals completion_ so a waiter wakes if the service
  // is torn down instead.
  while (static_cast<int32_t>(completedSerial_.load() - serial) < 0) {
    xSemaphoreTake(completion_, pdMS_TO_TICKS(20));
  }
}

bool ImageCacheService::ensureCached(const ImageBlock& block, const DecodeTarget& target, const int x, const int y) {
  if (block.hasValidCache()) return true;
  if (!task_) return block.hasValidCache();

  Job job;
  job.block = &block;
  job.target = target;
  job.x = x;
  job.y = y;
  waitForCompletion(postForeground(std::move(job)));
  return block.hasValidCache();
}

bool ImageCacheService::ensurePageCached(const Page& page, const DecodeTarget& target, const int xOffset,
                                         const int yOffset) {
  if (!page.hasImagesNeedingDecode()) return true;
  if (!task_) return !page.hasImagesNeedingDecode();

  Job job;
  job.borrowedPage = &page;
  job.target = target;
  job.x = xOffset;
  job.y = yOffset;
  waitForCompletion(postForeground(std::move(job)));
  return !page.hasImagesNeedingDecode();
}

void ImageCacheService::prefetchPage(std::unique_ptr<Page> page, const DecodeTarget& target, const int xOffset,
                                     const int yOffset) {
  if (!task_ || !page) return;

  Job job;
  job.ownedPage = std::move(page);
  job.target = target;
  job.x = xOffset;
  job.y = yOffset;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  job.valid = true;
  job.foreground = false;
  job.serial = nextSerial_++;
  jobs_[1] = std::move(job);
  xSemaphoreGive(mutex_);
  xTaskNotifyGive(task_);
}
