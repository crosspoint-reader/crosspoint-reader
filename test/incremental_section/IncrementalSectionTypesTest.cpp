#include <cassert>

#include "../../lib/Epub/Epub/IncrementalSectionCache.h"
#include "../../lib/Epub/Epub/IncrementalSectionTypes.h"

int main() {
  assert(IncrementalSection::CACHE_MAGIC == 0x43504953U);
  assert(IncrementalSection::CACHE_VERSION > 0);
  assert(IncrementalSection::PAGE_INDEX_RECORD_SIZE == 16);
  assert(sizeof(IncrementalSection::PageIndexRecord) == IncrementalSection::PAGE_INDEX_RECORD_SIZE);

  assert(IncrementalSection::knownPageCountFromIndexBytes(0) == 0);
  assert(IncrementalSection::knownPageCountFromIndexBytes(16) == 1);
  assert(IncrementalSection::knownPageCountFromIndexBytes(31) == 0);
  assert(IncrementalSection::knownPageCountFromIndexBytes(32) == 2);

  const auto paths = IncrementalSection::pathsForSection("/.crosspoint/epub_1/sections", 16);
  assert(paths.meta == "/.crosspoint/epub_1/sections/16.met");
  assert(paths.pages == "/.crosspoint/epub_1/sections/16.pag");
  assert(paths.index == "/.crosspoint/epub_1/sections/16.idx");
  assert(paths.anchors == "/.crosspoint/epub_1/sections/16.anc");

  IncrementalSection::LayoutCacheKey base{};
  base.cacheVersion = IncrementalSection::CACHE_VERSION;
  base.fontId = 1;
  base.lineCompression = 1.0F;
  base.extraParagraphSpacing = false;
  base.paragraphAlignment = 2;
  base.viewportWidth = 240;
  base.viewportHeight = 320;
  base.hyphenationEnabled = true;
  base.embeddedStyle = true;
  base.imageRendering = 1;
  base.focusReadingEnabled = true;

  auto same = base;
  assert(base.matches(same));

  auto changedFont = base;
  changedFont.fontId = 2;
  assert(!base.matches(changedFont));

  auto changedViewport = base;
  changedViewport.viewportHeight = 300;
  assert(!base.matches(changedViewport));

  auto changedHyphenation = base;
  changedHyphenation.hyphenationEnabled = false;
  assert(!base.matches(changedHyphenation));

  auto changedFocusReading = base;
  changedFocusReading.focusReadingEnabled = false;
  assert(!base.matches(changedFocusReading));

  IncrementalSection::PageIndexRecord record{};
  record.pageOffset = 10;
  record.pageLength = 20;
  record.paragraphIndex = 3;
  record.listItemIndex = 4;
  record.sourceByteOffset = 30;
  assert(record.pageOffset == 10);
  assert(record.pageLength == 20);
  assert(record.paragraphIndex == 3);
  assert(record.listItemIndex == 4);
  assert(record.sourceByteOffset == 30);
}
