#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

// Result of an index search — file location of a definition without reading it.
struct DictLocation {
  uint32_t offset = 0;  // byte offset in .dict data
  uint32_t size = 0;    // byte length in .dict data
  bool found = false;
};

// Slim StarDict reader: exact-match lookup with a mini stemming fallback.
//
// Expects /dictionaries/<folder>/<stem>.idx (uncompressed) plus <stem>.dict or
// <stem>.dict.dz. Lookups binary-search a lazily built sampled-offset sidecar
// (<stem>.qidx, byte offset of every SAMPLE_INTERVAL-th .idx entry), then
// linear-scan at most SAMPLE_INTERVAL entries. Everything streams from SD; no
// index is held in RAM.
class Dictionary {
 public:
  // Resolve the dictionary folder and validate its files. Rejects
  // dictionaries with 64-bit index offsets (idxoffsetbits=64 in .ifo).
  bool open(const char* folderName);
  bool isOpen() const { return !basePath.empty(); }

  // True when the .qidx sidecar is missing or stale — call buildIndex() first
  // so the UI can show an "Indexing…" message for the slow first pass.
  bool needsIndex();

  // One streaming pass over .idx writing the .qidx sidecar. yieldFn (optional)
  // is called every ~64KB consumed to feed the watchdog / repaint the UI.
  bool buildIndex(void (*yieldFn)(void*) = nullptr, void* ctx = nullptr);

  // Clean the word, look it up, and on a miss retry mini stem variants
  // (-'s/-s/-es/-ies/-ed/-ing). On a hit fills the definition text (capped at
  // MAX_DEFINITION_BYTES) and the headword as stored in the index.
  bool lookup(const char* word, std::string& definitionOut, std::string& matchedHeadwordOut);

  static std::string cleanWord(const char* word);

  static constexpr uint32_t MAX_DEFINITION_BYTES = 64 * 1024;

 private:
  static constexpr uint32_t SAMPLE_INTERVAL = 256;

  DictLocation locate(const char* target, std::string* matchedHeadwordOut);
  bool readDefinition(const DictLocation& location, std::string& out);
  static void stemVariants(const std::string& word, std::vector<std::string>& out);

  // Read a null-terminated word from an open file into buf (max bufSize-1
  // chars). Returns the number of characters read (excluding null), or -1 on
  // EOF/error. Over-long words are truncated but the stream stays in sync.
  static int readWordInto(HalFile& file, char* buf, size_t bufSize);

  std::string basePath;  // "/dictionaries/<folder>/<stem>", empty when not open
  bool hasPlainDict = false;

  // Shared scan buffer: lookups are single-threaded and this avoids a
  // 256-byte array on the stack of every locate() call.
  char wordBuf[256] = {};
};
