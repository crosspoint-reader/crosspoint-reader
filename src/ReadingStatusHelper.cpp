#include "ReadingStatusHelper.h"

#include <FsHelpers.h>
#include <HalStorage.h>

#include <functional>
#include <string>

ReadingStatus getReadingStatus(const std::string& filepath, const std::string& cacheDir) {
  // EPUB/XTC以外は常にUnread（アイコン対象外のため到達しないが安全策）
  const char* prefix;
  if (FsHelpers::hasEpubExtension(filepath)) {
    prefix = "epub_";
  } else if (FsHelpers::hasXtcExtension(filepath)) {
    prefix = "xtc_";
  } else {
    return ReadingStatus::Unread;
  }

  // progress.bin パスを構築
  std::string progressPath = cacheDir + "/" + prefix +
                             std::to_string(std::hash<std::string>{}(filepath)) + "/progress.bin";

  FsFile f;
  if (!Storage.openFileForRead("RSH", progressPath, f)) {
    return ReadingStatus::Unread;
  }

  // ファイル全体を読み取り（最大7バイト: EPUB新フォーマット）
  uint8_t data[7];
  int bytesRead = f.read(data, sizeof(data));
  f.close();

  if (bytesRead <= 0) {
    return ReadingStatus::Unread;
  }

  // 読了フラグの位置: EPUB=byte6, XTC=byte4
  int flagOffset = FsHelpers::hasEpubExtension(filepath) ? 6 : 4;

  if (bytesRead > flagOffset && data[flagOffset] == 1) {
    return ReadingStatus::Finished;
  }

  return ReadingStatus::Reading;
}
