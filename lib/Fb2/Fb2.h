#pragma once

#include <string>
#include <vector>

class Fb2 {
 public:
  struct SectionInfo {
    std::string title;
    size_t fileOffset;
    size_t length;
  };

  struct TocEntry {
    std::string title;
    int sectionIndex;
  };

 private:
  std::string filepath;
  std::string cachePath;
  std::string title;
  std::string author;
  std::string language;
  std::string coverBinaryId;
  std::vector<SectionInfo> sections;
  std::vector<TocEntry> tocEntries;
  bool loaded = false;

  bool parseMetadata();
  bool loadMetadataCache();
  bool saveMetadataCache() const;

 public:
  explicit Fb2(std::string filepath, const std::string& cacheDir);
  ~Fb2() = default;

  bool load(bool buildIfMissing = true);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;

  // Cover/thumbnail
  std::string getCoverBmpPath() const;
  bool generateCoverBmp() const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;

  // Sections (spine-like navigation)
  int getSectionCount() const;
  const SectionInfo& getSectionInfo(int index) const;
  size_t getBookSize() const;
  size_t getCumulativeSectionSize(int index) const;
  float calculateProgress(int currentSectionIndex, float currentSectionRead) const;

  // TOC
  int getTocCount() const;
  const TocEntry& getTocEntry(int index) const;
  int getTocIndexForSectionIndex(int sectionIndex) const;
  int getSectionIndexForTocIndex(int tocIndex) const;

  // Cover binary ID (for cover extractor)
  const std::string& getCoverBinaryId() const { return coverBinaryId; }
};
