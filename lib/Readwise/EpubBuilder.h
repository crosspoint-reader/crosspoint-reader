#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace readwise {

// Incremental CRC-32 (zip polynomial).
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);

struct ArticleMeta {
  std::string id;  // Readwise document id (also the EPUB identifier)
  std::string title;
  std::string author;
  std::string siteName;
  std::string sourceUrl;
  std::string language;  // BCP-47 primary tag, e.g. "en"
};

// Writes a single-chapter EPUB 3 (with an EPUB 2 NCX for older readers) as
// an uncompressed zip. The chapter body is produced elsewhere (streamed to a
// temp file first, so its size is known) and copied in by the caller through
// `copyBody`, which is called twice: once to compute the CRC, once to write.
//
//   EpubBuilder b(write);
//   b.writeArticle(meta, bodySize, copyBody);
//
// All output goes through `write`, in order, exactly once; no seeking.
class EpubBuilder {
 public:
  using Write = std::function<bool(const uint8_t* data, size_t len)>;
  // Streams the whole body through write(); may be called more than once.
  using CopyBody = std::function<bool(const Write& write)>;

  explicit EpubBuilder(Write write) : write_(std::move(write)) {}

  bool writeArticle(const ArticleMeta& meta, uint32_t bodySize, const CopyBody& copyBody);

  // The fixed text around the body; exposed for tests.
  static std::string chapterHead(const ArticleMeta& meta);
  static std::string chapterTail();

 private:
  struct Entry {
    const char* name;
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
  };

  bool addEntry(const char* name, const std::string& data);
  bool beginEntry(const char* name, uint32_t crc, uint32_t size);
  bool raw(const void* data, size_t len);
  bool finish();

  Write write_;
  uint32_t offset_ = 0;
  Entry entries_[8] = {};
  uint8_t entryCount_ = 0;
};

// Escapes text for XML element content and attribute values.
std::string xmlEscape(const std::string& s);

// Guesses an article's language from sanitized XHTML by counting common
// function words. Returns "en" when nothing stands out.
class LanguageGuesser {
 public:
  void feed(const char* data, size_t len);
  const char* result() const;

 private:
  void endWord();
  static constexpr size_t kWordMax = 8;
  static constexpr size_t kSampleWords = 1500;
  char word_[kWordMax + 1] = {};
  uint8_t wordLen_ = 0;
  bool wordTooLong_ = false;
  bool inTag_ = false;
  uint32_t words_ = 0;
  uint16_t scores_[8] = {};
};

}  // namespace readwise
