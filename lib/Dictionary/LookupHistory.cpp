#include "LookupHistory.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "DictionaryManager.h"

void LookupHistory::load() {
  count = 0;
  dirty = false;

  char buf[MAX_ENTRIES * DICT_WORD_MAX + MAX_ENTRIES];  // words + newlines
  size_t bytesRead = Storage.readFileToBuffer(HISTORY_FILE, buf, sizeof(buf));
  if (bytesRead == 0) {
    LOG_DBG("HIST", "No history file or empty");
    return;
  }

  // Parse lines
  int lineStart = 0;
  for (int i = 0; i <= static_cast<int>(bytesRead) && count < MAX_ENTRIES; ++i) {
    if (i == static_cast<int>(bytesRead) || buf[i] == '\n') {
      int lineLen = i - lineStart;
      if (lineLen > 0 && lineLen < DICT_WORD_MAX) {
        memcpy(words[count], buf + lineStart, lineLen);
        words[count][lineLen] = '\0';
        count++;
      }
      lineStart = i + 1;
    }
  }

  LOG_DBG("HIST", "Loaded %d history entries", count);
}

void LookupHistory::save() {
  if (!dirty) return;

  if (count == 0) {
    // Delete file if history is empty
    Storage.remove(HISTORY_FILE);
    dirty = false;
    LOG_DBG("HIST", "History cleared, file removed");
    return;
  }

  // Build output buffer: each word + newline
  char buf[MAX_ENTRIES * DICT_WORD_MAX + MAX_ENTRIES];
  int pos = 0;
  for (int i = 0; i < count; ++i) {
    int len = static_cast<int>(strlen(words[i]));
    memcpy(buf + pos, words[i], len);
    pos += len;
    buf[pos++] = '\n';
  }
  buf[pos] = '\0';

  Storage.ensureDirectoryExists("/.dictionaries");
  if (Storage.writeFile(HISTORY_FILE, String(buf))) {
    LOG_DBG("HIST", "Saved %d history entries", count);
  } else {
    LOG_ERR("HIST", "Failed to write history file");
  }
  dirty = false;
}

void LookupHistory::addWord(const char* word) {
  // Normalize the word first
  char normalized[DICT_WORD_MAX];
  if (!DictionaryManager::normalizeWord(word, normalized, DICT_WORD_MAX)) {
    return;  // Word normalizes to empty — skip
  }

  // Check for duplicate (case-insensitive, but words are already lowercased after normalize)
  for (int i = 0; i < count; ++i) {
    if (strncmp(words[i], normalized, DICT_WORD_MAX) == 0) {
      // Duplicate found — bump to front
      shiftDownAndInsertFront(i, normalized);
      dirty = true;
      return;
    }
  }

  // Not a duplicate — insert at front
  if (count >= MAX_ENTRIES) {
    // List full — drop oldest (last entry) by not incrementing count
    shiftDownAndInsertFront(count - 1, normalized);
  } else {
    shiftDownAndInsertFront(count, normalized);
    count++;
  }
  dirty = true;
}

void LookupHistory::removeWord(int index) {
  if (index < 0 || index >= count) return;

  // Shift entries after index up by one
  for (int i = index; i < count - 1; ++i) {
    memcpy(words[i], words[i + 1], DICT_WORD_MAX);
  }
  count--;
  words[count][0] = '\0';
  dirty = true;
}

void LookupHistory::clear() {
  if (count == 0) return;
  count = 0;
  memset(words, 0, sizeof(words));
  dirty = true;
}

const char* LookupHistory::getWord(int index) const {
  if (index < 0 || index >= count) return "";
  return words[index];
}

void LookupHistory::shiftDownAndInsertFront(int fromIndex, const char* word) {
  // Shift entries [0..fromIndex-1] down by one position to open slot 0
  for (int i = fromIndex; i > 0; --i) {
    memcpy(words[i], words[i - 1], DICT_WORD_MAX);
  }
  strncpy(words[0], word, DICT_WORD_MAX - 1);
  words[0][DICT_WORD_MAX - 1] = '\0';
}
