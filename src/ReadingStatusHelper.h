#pragma once

#include <cstdint>
#include <string>

enum class ReadingStatus : uint8_t {
  Unread,    // progress.bin が存在しない
  Reading,   // progress.bin が存在し、読了フラグなし
  Finished   // progress.bin が存在し、読了フラグあり
};

// ファイルパスからSDカード上のキャッシュを確認し、読書状態を返す。
// filepath: 書籍ファイルの絶対パス（例: "/books/sample.epub"）
// cacheDir: キャッシュルート（通常 "/.crosspoint"）
ReadingStatus getReadingStatus(const std::string& filepath, const std::string& cacheDir);
