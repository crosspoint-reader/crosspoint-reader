#pragma once

#include <Print.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class Epub {
 public:
  struct SpineEntry {
    std::string href;
  };

  explicit Epub(std::vector<std::string> chapters, const size_t streamChunkSize = 0)
      : chapters_(std::move(chapters)), streamChunkSize_(streamChunkSize) {}

  int getSpineItemsCount() const { return static_cast<int>(chapters_.size()); }

  SpineEntry getSpineItem(const int spineIndex) const {
    if (spineIndex < 0 || spineIndex >= getSpineItemsCount()) {
      return {};
    }
    return {"chapter" + std::to_string(spineIndex) + ".xhtml"};
  }

  bool readItemContentsToStream(const std::string& href, Print& output, const size_t requestedChunkSize,
                                const bool allowEarlyStop = false) const {
    const int spineIndex = parseSpineIndex(href);
    if (spineIndex < 0) {
      return false;
    }

    const std::string& chapter = chapters_[spineIndex];
    const size_t chunkSize = streamChunkSize_ > 0 ? streamChunkSize_ : std::max<size_t>(requestedChunkSize, 1);
    for (size_t offset = 0; offset < chapter.size(); offset += chunkSize) {
      const size_t count = std::min(chunkSize, chapter.size() - offset);
      const size_t written = output.write(reinterpret_cast<const uint8_t*>(chapter.data() + offset), count);
      if (written != count) {
        return allowEarlyStop;
      }
    }
    return true;
  }

  bool getItemSize(const std::string& href, size_t* size) const {
    const int spineIndex = parseSpineIndex(href);
    if (spineIndex < 0 || !size) {
      return false;
    }
    *size = chapters_[spineIndex].size();
    return true;
  }

  size_t getBookSize() const {
    size_t total = 0;
    for (const auto& chapter : chapters_) {
      total += chapter.size();
    }
    return total;
  }

  size_t getCumulativeSpineItemSize(const int spineIndex) const {
    if (spineIndex < 0) {
      return 0;
    }
    size_t total = 0;
    for (int i = 0; i <= spineIndex && i < getSpineItemsCount(); i++) {
      total += chapters_[i].size();
    }
    return total;
  }

  float calculateProgress(const int spineIndex, const float intraSpineProgress) const {
    const size_t total = getBookSize();
    if (total == 0 || spineIndex < 0 || spineIndex >= getSpineItemsCount()) {
      return 0.0f;
    }
    const size_t before = spineIndex > 0 ? getCumulativeSpineItemSize(spineIndex - 1) : 0;
    const float intra = std::clamp(intraSpineProgress, 0.0f, 1.0f);
    return (static_cast<float>(before) + static_cast<float>(chapters_[spineIndex].size()) * intra) /
           static_cast<float>(total);
  }

 private:
  int parseSpineIndex(const std::string& href) const {
    static constexpr char prefix[] = "chapter";
    static constexpr char suffix[] = ".xhtml";
    if (!href.starts_with(prefix) || !href.ends_with(suffix)) {
      return -1;
    }
    const std::string digits =
        href.substr(sizeof(prefix) - 1, href.size() - (sizeof(prefix) - 1) - (sizeof(suffix) - 1));
    if (digits.empty()) {
      return -1;
    }
    int value = 0;
    for (const char digit : digits) {
      if (digit < '0' || digit > '9') {
        return -1;
      }
      value = value * 10 + digit - '0';
    }
    return value < getSpineItemsCount() ? value : -1;
  }

  std::vector<std::string> chapters_;
  size_t streamChunkSize_ = 0;
};
