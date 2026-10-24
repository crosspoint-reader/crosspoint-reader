#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <string>

#include "converters/DecodeTarget.h"

class ImageBlock;
class Page;

// Single-owner image decoder. All decode-to-.pxc work runs on one worker task;
// the render path never decodes inline, it only asks this service to produce a
// cache and waits. Because there is exactly one writer, two decoders can never
// race on the same .pxc file.
//
// Foreground requests (a page being rendered) wait for completion. Background
// requests (the reader's idle prefetch of the next page) run when the worker is
// otherwise idle and are not waited on, so the cache is simply ready later.
class ImageCacheService {
 public:
  static ImageCacheService& getInstance();

  // Worker task lifecycle. start()/stop() are idempotent.
  void start();
  void stop();

  // Wait until the page's still-uncached images have a .pxc. Returns the job's
  // result (true when the page no longer needs a decode). The wait is
  // unbounded until this job finishes: the job borrows the Page, so returning
  // early (e.g. on a timeout) could outlive the caller's Page and leave the
  // worker on freed memory.
  bool ensurePageCached(const Page& page, const DecodeTarget& target, int xOffset, int yOffset);

  // Wait until this image has a .pxc. Returns the job's result (a decode that
  // wrote no cache reports false). Same borrow-lifetime guarantee as
  // ensurePageCached.
  bool ensureCached(const ImageBlock& block, const DecodeTarget& target, int x, int y);

  // Queue the next page's images for decode without waiting. Any previous
  // background job is replaced.
  void prefetchPage(std::unique_ptr<Page> page, const DecodeTarget& target, int xOffset, int yOffset);

 private:
  ImageCacheService() = default;
  ImageCacheService(const ImageCacheService&) = delete;
  ImageCacheService& operator=(const ImageCacheService&) = delete;

  struct Job {
    bool valid = false;
    bool foreground = false;
    uint32_t serial = 0;
    const Page* borrowedPage = nullptr;  // foreground page request
    std::unique_ptr<Page> ownedPage;     // background page request
    const ImageBlock* block = nullptr;   // foreground single-image request
    DecodeTarget target{};
    int x = 0;
    int y = 0;
  };

  static void taskTrampoline(void* param);
  void run();
  bool runJob(Job& job);
  uint32_t postForeground(Job&& job);
  bool waitForCompletion(uint32_t serial);

  SemaphoreHandle_t mutex_ = nullptr;
  SemaphoreHandle_t completion_ = nullptr;
  SemaphoreHandle_t stopped_ = nullptr;
  TaskHandle_t task_ = nullptr;
  Job jobs_[2];  // 0 = foreground, 1 = background
  uint32_t nextSerial_ = 1;
  std::atomic<uint32_t> completedSerial_{0};
  // Job results keyed by serial. The worker writes a slot before advancing
  // completedSerial_, so a woken waiter (which acquires via that load) reads
  // its own job's result; 8 slots cannot be reused within the waiter's 20 ms
  // poll interval. stop() clears the ring so a dropped job never reports a
  // stale success.
  bool results_[8] = {};
  std::atomic<bool> stopRequested_{false};
};
