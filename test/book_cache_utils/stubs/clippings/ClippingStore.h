#pragma once

#include <cstdint>
#include <string>

namespace ClippingCodec {
struct BookMetadata {
  std::string title;
  std::string author;
  std::string bookType = "epub";
};
}  // namespace ClippingCodec

class ClippingStore {
 public:
  enum class LoadResult : uint8_t { Ready };
  enum class RekeyResult : uint8_t { Rekeyed, Prepared, Unchanged, IoError };

  LoadResult loadForBook(const std::string&, const std::string&, const std::string&, const std::string& = "epub") {
    loaded_ = true;
    return LoadResult::Ready;
  }
  static bool removeFilesForBook(const std::string&, const std::string& = "epub") { return true; }
  static bool hasFilesForBook(const std::string&, const std::string& = "epub") { return false; }
  static bool quarantineFilesForBook(const std::string&, const std::string&, const std::string& = "epub") {
    return true;
  }
  bool isLoaded() const { return loaded_; }
  const ClippingCodec::BookMetadata& book() const { return book_; }
  RekeyResult prepareRekeyForBook(const std::string&, const std::string&, const std::string&,
                                  const std::string& = "epub") {
    return RekeyResult::Prepared;
  }
  RekeyResult finalizePreparedRekey() { return RekeyResult::Rekeyed; }
  bool cancelPreparedRekey() { return true; }

 private:
  bool loaded_ = false;
  ClippingCodec::BookMetadata book_;
};
