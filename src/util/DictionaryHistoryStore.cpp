#include "DictionaryHistoryStore.h"

#include <AtomicFile.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "DictionaryQuery.h"

namespace {
constexpr char HISTORY_PATH[] = "/.crosspoint/dictionary_history.txt";
constexpr char HEADER[] = "CROSSVI_DICT_HISTORY_V1\n";
}  // namespace

DictionaryHistoryStore& DictionaryHistoryStore::getInstance() {
  static DictionaryHistoryStore instance;
  return instance;
}

bool DictionaryHistoryStore::validate(const uint8_t* data, size_t size, void*) {
  if (!data || size < sizeof(HEADER) - 1 || size > MAX_FILE_BYTES) return false;
  if (memcmp(data, HEADER, sizeof(HEADER) - 1) != 0) return false;
  size_t lineStart = sizeof(HEADER) - 1;
  size_t count = 0;
  for (size_t index = lineStart; index <= size; ++index) {
    if (index != size && data[index] != '\n') continue;
    if (index == lineStart) {
      if (index == size) break;
      return false;
    }
    const size_t length = index - lineStart;
    if (length > MAX_QUERY_BYTES || ++count > MAX_ENTRIES) return false;
    const std::string line(reinterpret_cast<const char*>(data + lineStart), length);
    if (DictionaryQuery::clean(line) != line) return false;
    lineStart = index + 1;
  }
  return true;
}

bool DictionaryHistoryStore::parse(const std::string& data) {
  entries_.clear();
  size_t start = sizeof(HEADER) - 1;
  while (start < data.size()) {
    const size_t end = data.find('\n', start);
    const size_t finish = end == std::string::npos ? data.size() : end;
    if (finish > start) entries_.emplace_back(data.substr(start, finish - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return true;
}

std::string DictionaryHistoryStore::serialize() const {
  std::string data = HEADER;
  for (const auto& entry : entries_) {
    data += entry;
    data.push_back('\n');
  }
  return data;
}

bool DictionaryHistoryStore::load() {
  if (loaded_) return writable_;
  loaded_ = true;
  std::string data;
  const auto status = AtomicFile::load(HISTORY_PATH, data, MAX_FILE_BYTES, validate);
  if (status == AtomicFile::LoadStatus::Missing) {
    writable_ = true;
    return true;
  }
  if (status != AtomicFile::LoadStatus::Primary && status != AtomicFile::LoadStatus::Backup &&
      status != AtomicFile::LoadStatus::Temp) {
    writable_ = false;
    LOG_ERR("DHIST", "History is invalid or unreadable; preserving it read-only");
    return false;
  }
  writable_ = parse(data);
  return writable_;
}

const std::vector<std::string>& DictionaryHistoryStore::entries() {
  load();
  return entries_;
}

void DictionaryHistoryStore::record(const std::string& query) {
  load();
  if (!writable_) return;
  const std::string cleaned = DictionaryQuery::clean(query);
  if (cleaned.empty() || cleaned.size() > MAX_QUERY_BYTES) return;
  entries_.erase(std::remove(entries_.begin(), entries_.end(), cleaned), entries_.end());
  entries_.insert(entries_.begin(), cleaned);
  if (entries_.size() > MAX_ENTRIES) entries_.resize(MAX_ENTRIES);
  dirty_ = true;
}

bool DictionaryHistoryStore::flush() {
  load();
  if (!writable_) return false;
  if (!dirty_) return true;
  Storage.mkdir("/.crosspoint");
  const std::string data = serialize();
  const auto status = AtomicFile::save(HISTORY_PATH, data, MAX_FILE_BYTES, validate);
  if (status == AtomicFile::SaveStatus::Saved || status == AtomicFile::SaveStatus::Unchanged) {
    dirty_ = false;
    return true;
  }
  if (status == AtomicFile::SaveStatus::InvalidExistingState) writable_ = false;
  return false;
}

bool DictionaryHistoryStore::clear() {
  load();
  if (!writable_) return false;
  entries_.clear();
  dirty_ = true;
  return flush();
}
