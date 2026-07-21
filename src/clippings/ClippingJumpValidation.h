#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "activities/ActivityResult.h"
#include "clippings/ClippingCodec.h"

namespace ClippingJumpValidation {

struct StoreSnapshot {
  const ClippingCodec::BookMetadata& book;
  std::string_view storePath;
  ClippingCodec::Format format;
  uint32_t fileLength;
  const std::vector<ClippingCodec::ClippingMetadata>& entries;
  int spineCount;
  uint32_t textCrc32;
};

inline bool isExact(const ClippingJumpResult& request, const StoreSnapshot& current) {
  if (current.format != ClippingCodec::Format::Current ||
      request.storeFormat != static_cast<uint8_t>(ClippingCodec::Format::Current)) {
    return false;
  }
  if (request.bookTitle != current.book.title || request.bookAuthor != current.book.author ||
      request.bookPath != current.book.path || request.bookType != current.book.bookType ||
      request.storePath != current.storePath || request.storeFileLength != current.fileLength) {
    return false;
  }
  const std::string canonicalStore = ClippingCodec::filePathForBook(request.bookPath, request.bookType);
  if (canonicalStore.empty() || canonicalStore != request.storePath) return false;
  if (request.clippingIndex >= current.entries.size() || current.spineCount <= 0 ||
      request.spineIndex >= current.spineCount) {
    return false;
  }

  const auto& clipping = current.entries[request.clippingIndex];
  if (request.spineIndex != clipping.spineIndex || request.startPage != clipping.startPage ||
      request.endPage != clipping.endPage || request.pageCount != clipping.pageCount ||
      request.startWordIndex != clipping.startWordIndex || request.endWordIndex != clipping.endWordIndex ||
      request.wordCount != clipping.wordCount || request.paragraphIndex != clipping.paragraphIndex ||
      request.timestamp != clipping.timestamp || request.textOffset != clipping.textOffset ||
      request.textLength != clipping.textLength || request.pageFingerprint != clipping.pageFingerprint ||
      request.chapterTitle != clipping.chapterTitle || request.textCrc32 != current.textCrc32) {
    return false;
  }

  // A page fingerprint identifies one rendered page only. Multi-page and
  // legacy entries remain readable but cannot issue an exact navigation jump.
  return request.pageFingerprint != 0 && request.startPage == request.endPage && request.pageCount > 0 &&
         request.startPage < request.pageCount && request.wordCount > 0 && request.textLength > 0;
}

}  // namespace ClippingJumpValidation
