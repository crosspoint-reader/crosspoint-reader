#pragma once

#include <HalStorage.h>

#include <cstddef>

#include "HtmlToXhtml.h"
#include "ZipStreamWriter.h"

/**
 * Builds a minimal but spec-valid EPUB 3 on the SD card by streaming article
 * HTML through the whitelist sanitizer into a stored ZIP container:
 *
 *   mimetype (first entry, stored) -> META-INF/container.xml ->
 *   OEBPS/content.xhtml (streamed) -> OEBPS/content.opf -> OEBPS/nav.xhtml
 *
 * Metadata (title/author) may only be known after the HTML has streamed past,
 * so the OPF/nav entries are written last; ZIP entry order other than
 * "mimetype first" is irrelevant to readers.
 */
class EpubComposer {
 public:
  explicit EpubComposer(const char* sdPath);

  // Creates/truncates the target file and writes the fixed front entries.
  bool begin();
  // Starts the content entry and emits the XHTML skeleton head.
  bool beginContent();
  // Feeds raw article HTML; sanitized output goes into the ZIP.
  bool contentChunk(const char* data, size_t len);
  // Closes the content entry, writes OPF + nav, finalizes the archive.
  bool endContent(const char* title, const char* author, const char* docId);
  // Closes and deletes the partial file.
  void abort();

 private:
  static int sinkWrite(void* ctx, const char* data, size_t len);

  static constexpr size_t PATH_SIZE = 160;

  char path_[PATH_SIZE];
  HalFile file_;
  ZipStreamWriter zip_;
  HtmlToXhtml html_;
};
