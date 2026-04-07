#pragma once

#include <string>
#include <vector>
#include <HalStorage.h>

struct AozoraBookEntry {
  int workId;
  char title[64];
  char author[32];
  char filename[80];
};

class AozoraIndexManager {
 public:
  static constexpr const char* AOZORA_DIR = "/Aozora";
  static constexpr const char* INDEX_PATH = "/Aozora/.aozora_index.json";

  bool loadAndPurge();
  bool isDownloaded(int workId) const;
  bool addEntry(int workId, const char* title, const char* author, const char* filename);
  bool removeEntry(int workId);
  const std::vector<AozoraBookEntry>& entries() const { return entries_; }
  /** 著者名/workId_タイトル.epub 形式の相対パスを生成 */
  static std::string makeRelativePath(int workId, const char* title, const char* author);
  static bool ensureDirectory();
  /** 著者名サブディレクトリを含む保存先を確保 */
  static bool ensureAuthorDirectory(const char* author);

 private:
  std::vector<AozoraBookEntry> entries_;
  bool saveIndex() const;
};
