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
  // Set when the search was cut short by an .idx open or seek failure rather than
  // reaching a verdict, so a failed search isn't reported as a genuine miss.
  bool readError = false;
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
  // Why a lookup did not return a definition — so the UI can tell a genuine
  // miss apart from a real failure, and name the failure.
  enum class LookupResult : uint8_t {
    Found,       // hit — definition filled
    NotFound,    // the word is genuinely not in the dictionary
    LowMemory,   // found, but an allocation failed: the ~32KB .dict.dz inflate
                 // window / chunk buffer, or the definition text buffer, couldn't
                 // be obtained from the fragmented heap. THIS is the "stopped
                 // finding words until restart" case — definitively memory.
    Decompress,  // found, but decompression genuinely failed (corrupt/truncated
                 // .dict.dz — a read/inflate error, not a memory shortage)
    ReadError,   // found, but a file open/bounds/IO error prevented reading it
  };

  // Resolve the dictionary folder and validate its files. Rejects
  // dictionaries with 64-bit index offsets (idxoffsetbits=64 in .ifo).
  bool open(const char* folderName);
  bool isOpen() const { return !basePath.empty(); }

  // True when the .qidx sidecar is missing or stale — call buildIndex() first
  // so the UI can show an "Indexing…" message for the slow first pass.
  bool needsIndex();

  // Why an index build failed — the scan buffer is a heap allocation, so the
  // same fragmentation that breaks lookups can break indexing, and it deserves
  // the same "Not enough memory" rather than a generic error.
  enum class IndexResult : uint8_t {
    Ok,
    LowMemory,  // the scan buffer couldn't be allocated
    ReadError,  // .idx open/read or .qidx write failure
  };

  // One streaming pass over .idx writing the .qidx sidecar. yieldFn (optional)
  // is called every ~64KB consumed to feed the watchdog / repaint the UI.
  // *outResult (if provided) reports why a failed build failed.
  bool buildIndex(void (*yieldFn)(void*) = nullptr, void* ctx = nullptr, IndexResult* outResult = nullptr);

  // Clean the word, look it up, and on a miss retry mini stem variants
  // (-'s/-s/-es/-ies/-ed/-ing). On a hit fills the definition text (capped at
  // MAX_DEFINITION_BYTES) and the headword as stored in the index. Returns true
  // on a hit. *outResult (if provided) reports the precise outcome so the UI can
  // distinguish a genuine miss from a decompression / low-memory / read failure.
  bool lookup(const char* word, std::string& definitionOut, std::string& matchedHeadwordOut,
              LookupResult* outResult = nullptr);

  static std::string cleanWord(const char* word);

  static constexpr uint32_t MAX_DEFINITION_BYTES = 64 * 1024;

 private:
  static constexpr uint32_t SAMPLE_INTERVAL = 256;

  // Longest "<basePath><suffix>" the lookup path builds, rounded up. basePath is
  // "/dictionaries/<folder>/<stem>" (14 fixed chars) and the longest suffix is
  // ".dict.dz", leaving ~137 chars for folder + stem — far beyond any real
  // dictionary. open() rejects anything that would not fit, so the hot path
  // cannot fail on length. Kept under the 256-byte stack-local guideline.
  static constexpr size_t PATH_BUF_BYTES = 160;

  // Length of the longest suffix appended to basePath (".dict.dz"); used for
  // the open()-time length check.
  static constexpr size_t LONGEST_SUFFIX_LEN = sizeof(".dict.dz") - 1;

  // Compose "<basePath><suffix>" into a caller-supplied stack buffer. The
  // lookup path runs this instead of `basePath + suffix` so path construction
  // costs no transient heap — see LookupSession. (A lookup still allocates
  // elsewhere: cleanWord(), stemVariants() and the matched headword.) False
  // (and logs) when the path would not fit, which open() has already ruled out.
  bool buildPath(char* buf, size_t bufSize, const char* suffix) const;

  // The .idx / .qidx handles shared by every locate() call in one lookup. A
  // lookup probes up to ~5 stem variants; opening the two files per probe cost
  // ~10 SD opens and ~10 std::string path temporaries per word, churning the
  // same heap whose fragmentation makes lookups fail mid-session. Opened once
  // per lookup instead, with the paths built via buildPath().
  struct LookupSession {
    HalFile idx;
    HalFile qidx;
    uint32_t idxSize = 0;
    uint32_t sampleCount = 0;  // 0 when the sidecar is absent, stale or empty
  };

  // Open .idx (required) and .qidx (optional — locate() falls back to a full
  // scan without it). False when the dictionary is closed or .idx won't open.
  bool openSession(LookupSession& session);

  DictLocation locate(LookupSession& session, const char* target, std::string* matchedHeadwordOut);
  // Read the definition at location. On failure returns false and, if outResult
  // is given, sets it to the specific reason (Decompress / LowMemory / ReadError).
  bool readDefinition(const DictLocation& location, std::string& out, LookupResult* outResult = nullptr);
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
