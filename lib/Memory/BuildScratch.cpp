#include "BuildScratch.h"

#include <Logging.h>

#include <atomic>

namespace buildscratch {
namespace {
uint8_t* block = nullptr;
size_t blockLen = 0;
// atomic exchange so an opportunistic claim from another task can never
// double-hand-out the block (single core, but FreeRTOS preempts).
std::atomic<bool> claimed{false};
}  // namespace

void lend(uint8_t* buf, const size_t len) {
  if (block) {
    LOG_ERR("SCR", "Build scratch lent twice; ignoring second lend");
    return;
  }
  block = buf;
  blockLen = len;
  claimed.store(false);
}

void reclaim() {
  if (claimed.load()) {
    // A consumer still holds the block. The storage stays valid (it is the
    // framebuffer allocation, never freed) but its contents are about to be
    // clobbered; the consumer's output will be garbage. Loud log so a
    // lifetime bug is visible instead of a silent corrupt decode.
    LOG_ERR("SCR", "Build scratch reclaimed while still claimed");
  }
  block = nullptr;
  blockLen = 0;
  claimed.store(false);
}

uint8_t* claim(const size_t minLen, size_t* lenOut) {
  if (!block || blockLen < minLen) return nullptr;
  bool expected = false;
  if (!claimed.compare_exchange_strong(expected, true)) return nullptr;
  if (lenOut) *lenOut = blockLen;
  return block;
}

void release(const uint8_t* p) {
  if (p && p == block) claimed.store(false);
}

}  // namespace buildscratch
