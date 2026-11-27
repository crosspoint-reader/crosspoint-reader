#include "DictionaryHistory.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>

namespace dicthistory {
namespace {

constexpr char DIR[] = "/.crosspoint";
constexpr char PATH[] = "/.crosspoint/dictionary_history.txt";
constexpr char TMP_PATH[] = "/.crosspoint/dictionary_history.txt.tmp";
// Far longer than any real word; a line that does not fit is treated as corrupt and skipped.
constexpr size_t MAX_LINE_BYTES = 96;

bool sameWord(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

bool readFrom(const char* path, std::vector<std::string>& out) {
  HalFile file;
  if (!Storage.exists(path) || !Storage.openFileForRead("DHIST", path, file)) return false;
  std::string line;
  bool overlong = false;
  char buf[128];
  int got;
  while (out.size() < MAX_WORDS && (got = file.read(buf, sizeof(buf))) > 0) {
    for (int i = 0; i < got && out.size() < MAX_WORDS; ++i) {
      const char c = buf[i];
      if (c == '\n') {
        if (!line.empty() && !overlong) out.push_back(std::move(line));
        line.clear();
        overlong = false;
      } else if (line.size() < MAX_LINE_BYTES) {
        line.push_back(c);
      } else {
        overlong = true;
      }
    }
  }
  if (!line.empty() && !overlong && out.size() < MAX_WORDS) out.push_back(std::move(line));
  return true;
}

}  // namespace

void load(std::vector<std::string>& out) {
  out.clear();
  out.reserve(MAX_WORDS);
  // A power cut between replaceFile's remove and rename leaves only the .tmp.
  if (!readFrom(PATH, out)) readFrom(TMP_PATH, out);
}

void add(const std::string& word) {
  const auto first = word.find_first_not_of(" \t");
  if (first == std::string::npos) return;
  const std::string trimmed = word.substr(first, word.find_last_not_of(" \t") - first + 1);

  std::vector<std::string> words;
  load(words);
  // Already the newest entry: nothing to rewrite.
  if (!words.empty() && words.front() == trimmed) return;
  words.erase(std::remove_if(words.begin(), words.end(), [&](const std::string& w) { return sameWord(w, trimmed); }),
              words.end());
  // Drop the oldest first so the insert fits the reserved capacity.
  if (words.size() >= MAX_WORDS) words.pop_back();
  words.insert(words.begin(), trimmed);

  if (!Storage.ensureDirectoryExists(DIR)) return;
  {
    HalFile file;
    if (!Storage.openFileForWrite("DHIST", TMP_PATH, file)) return;
    bool complete = true;
    for (const auto& w : words) {
      complete = complete && file.write(w.data(), w.size()) == w.size() && file.write(static_cast<uint8_t>('\n')) == 1;
    }
    if (!file.close() || !complete) {
      Storage.remove(TMP_PATH);
      LOG_ERR("DHIST", "Short write saving dictionary history");
      return;
    }
  }
  if (!Storage.replaceFile(TMP_PATH, PATH)) LOG_ERR("DHIST", "Cannot replace dictionary history");
}

}  // namespace dicthistory
