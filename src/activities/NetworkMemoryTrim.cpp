#include "NetworkMemoryTrim.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include "GlobalBookmarkIndex.h"
#include "SdCardFontGlobals.h"
#include "activities/Activity.h"  // and ActivityManager, which needs the complete Activity

void trimMemoryForNetworkSession(const GfxRenderer& renderer, const char* logTag) {
  renderer.releaseGlyphCaches();
  // Reclaim the buffer first if the reader lent it out for a background page build. A lent
  // buffer reports hasSecondaryBuffer() == false, so without this the trim below would quietly
  // do nothing AND the later releaseFrameBuffers() would bail out on _secondaryLent — leaving
  // ~52 KB held for the whole session by the one code path that exists to free it. Returns
  // false and costs nothing when nothing is lent, which is the normal case.
  if (renderer.returnSecondaryBuffer()) {
    LOG_DBG(logTag, "Reclaimed a lent secondary framebuffer before trimming");
  }

  if (renderer.hasSecondaryBuffer()) {
    // Seed the controller baseline while frameBufferActive is still valid.
    renderer.syncRedRamFromFrameBuffer();
    if (renderer.releaseSecondaryBuffer()) {
      LOG_DBG(logTag, "Released secondary framebuffer before network session (~52 KB contiguous)");
      renderer.setSingleBufferFastDiff(true);
    }
  }
}

void releaseMemoryForDownload(const GfxRenderer& renderer, const char* logTag) {
  const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
  const size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  trimMemoryForNetworkSession(renderer, logTag);
  // A loaded SD font: intervals, kern and ligature tables, up to ~60 KB. Usually already gone,
  // since leaving the reader unloads it.
  unloadSdFontIfLoaded();
  GLOBAL_BOOKMARKS.unload();
  const int buried = activityManager.releaseBuriedActivityState();
  LOG_DBG(logTag, "Released for download (%d buried screen(s)): free %u -> %u B, largest block %u -> %u B", buried,
          static_cast<unsigned>(freeBefore), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)),
          static_cast<unsigned>(largestBefore),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
}
