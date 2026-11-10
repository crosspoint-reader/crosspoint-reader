#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#if defined(ARDUINO_ARCH_ESP32)
#include <sdkconfig.h>
#endif

inline constexpr size_t CLIPPING_CHAPTER_TITLE_MAX = 48;
// Clipping text lives on the SD card rather than in every in-memory clipping
// record. Match the reader's bounded selection-text budget so previews retain
// a complete multi-paragraph selection without growing the saved-item index.
inline constexpr size_t CLIPPING_TEXT_MAX = 4U * 1024U;
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
inline constexpr uint16_t CLIPPING_MAX_PER_BOOK = 1024;
#else
inline constexpr uint16_t CLIPPING_MAX_PER_BOOK = 256;
#endif

struct Clipping {
  uint16_t spineIndex = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pageCount = 1;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t wordCount = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  uint32_t timestamp = 0;
  uint32_t layoutSignature = 0;
  uint32_t startOffset = UINT32_MAX;
  uint32_t endOffset = UINT32_MAX;
  uint64_t syncRevision = 0;
  char id[65] = {};
  bool pendingUpload = true;
  uint32_t textOffset = 0;
  uint16_t textLength = 0;
  char chapterTitle[CLIPPING_CHAPTER_TITLE_MAX] = {};
};

class ClippingStore {
 public:
  enum class AddResult : uint8_t {
    Added,
    LimitReached,
    SaveFailed,
  };

  static ClippingStore& getInstance() { return instance; }

  bool loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                   const std::string& bookType);
  void unload();

  AddResult addClipping(uint16_t spineIndex, uint16_t startPage, uint16_t endPage, uint16_t pageCount,
                        uint16_t startWordIndex, uint16_t endWordIndex, uint16_t wordCount, const char* chapterTitle,
                        uint16_t paragraphIndex, const std::string& text, uint32_t layoutSignature,
                        uint32_t startOffset = UINT32_MAX, uint32_t endOffset = UINT32_MAX);
  bool removeClippingAt(size_t index);
  bool saveToFile();
  bool prepareSync();
  bool finishUploads();
  bool applyRemote(const Clipping& clipping, const std::string& text, bool deleted);
  bool nextDeletion(uint32_t& offset, char (&id)[65]) const;
  bool finishDeletions();

  bool hasClippings() const { return clippingSize != 0; }
  size_t clippingCount() const { return clippingSize; }
  const Clipping* clippingAt(size_t index) const;
  bool readClippingPreview(size_t index, char* out, size_t outSize, size_t& outLength) const;
  bool readClippingText(size_t index, std::string& out) const;
  bool readClippingText(const Clipping& clipping, std::string& out) const;

  static bool deleteForFilePath(const std::string& filePath, const std::string& bookType);
  // Move a book or folder together with its clipping stores and deletion journals.
  static bool moveBook(const std::string& from, const std::string& to);

 private:
  static ClippingStore instance;

  // Allocate only populated blocks; growing the index never duplicates existing records.
  static constexpr size_t CLIPPINGS_PER_BLOCK = 16;
  std::array<std::unique_ptr<Clipping[]>, (CLIPPING_MAX_PER_BOOK + CLIPPINGS_PER_BLOCK - 1) / CLIPPINGS_PER_BLOCK>
      clippingBlocks;
  Clipping& clippingRef(size_t index) {
    return clippingBlocks[index / CLIPPINGS_PER_BLOCK][index % CLIPPINGS_PER_BLOCK];
  }
  size_t clippingSize = 0;
  size_t clippingCapacity = 0;
  bool loaded = false;
  std::string bookFilePath;
  std::string bookTitle;
  std::string bookAuthor;
  std::string storeFilePath;
  bool dirty = false;

  bool reserveClippings(size_t count);
  bool readFromFile();
  bool writeToFile(const std::string* replacementText = nullptr, size_t replacementIndex = SIZE_MAX);
};

inline bool clippingStoredRangeMatchesLayout(const Clipping& clipping, const uint16_t currentPageCount,
                                             const uint32_t currentLayoutSignature) {
  if (clipping.pageCount != currentPageCount) return false;
  // A zero signature belongs to legacy data and cannot safely address page-local
  // word ordinals after relayout. It remains viewable in the clipping list, but
  // is not rendered as a highlight until it can be anchored by a newer format.
  return clipping.layoutSignature != 0 && currentLayoutSignature != 0 &&
         clipping.layoutSignature == currentLayoutSignature;
}

inline bool clippingContainsWord(const Clipping& clipping, const uint16_t spineIndex, const uint16_t page,
                                 const uint16_t pageCount, const uint32_t layoutSignature, const uint16_t wordIndex,
                                 const uint32_t sourceStart, const uint32_t sourceEnd) {
  if (clipping.spineIndex != spineIndex) return false;
  if (clipping.startOffset != UINT32_MAX && clipping.endOffset != UINT32_MAX) {
    return sourceStart != UINT32_MAX && sourceEnd > clipping.startOffset && sourceStart < clipping.endOffset;
  }
  return clippingStoredRangeMatchesLayout(clipping, pageCount, layoutSignature) && page >= clipping.startPage &&
         page <= clipping.endPage && (page != clipping.startPage || wordIndex >= clipping.startWordIndex) &&
         (page != clipping.endPage || wordIndex <= clipping.endWordIndex);
}

#define CLIPPINGS ClippingStore::getInstance()
