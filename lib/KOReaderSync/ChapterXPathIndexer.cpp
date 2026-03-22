#include "ChapterXPathIndexer.h"

#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ---- Utility ----

std::string toLowerStr(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool isSkippableTag(const std::string& tag) { return tag == "head" || tag == "script" || tag == "style"; }

bool isWhitespaceOnly(const XML_Char* text, const int len) {
  for (int i = 0; i < len; i++) {
    if (!std::isspace(static_cast<unsigned char>(text[i]))) {
      return false;
    }
  }
  return true;
}

size_t countVisibleBytes(const XML_Char* text, const int len) {
  size_t count = 0;
  for (int i = 0; i < len; i++) {
    if (!std::isspace(static_cast<unsigned char>(text[i]))) {
      count++;
    }
  }
  return count;
}

// Canonicalize a KOReader XPath for comparison:
// - remove whitespace, lowercase, strip /text() with optional char offset.
std::string normalizeXPath(const std::string& input) {
  if (input.empty()) {
    return "";
  }

  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isspace(uc)) {
      continue;
    }
    out.push_back(static_cast<char>(std::tolower(uc)));
  }

  // Strip /text() and any optional character offset suffix (e.g. /text().327).
  const std::string textTag = "/text()";
  const size_t textPos = out.rfind(textTag);
  if (textPos != std::string::npos) {
    const size_t afterText = textPos + textTag.size();
    if (afterText == out.size() || out[afterText] == '.') {
      out.erase(textPos);
    }
  }

  while (!out.empty() && out.back() == '/') {
    out.pop_back();
  }

  return out;
}

std::string removeIndices(const std::string& xpath) {
  std::string out;
  out.reserve(xpath.size());
  bool inBracket = false;
  for (char c : xpath) {
    if (c == '[') {
      inBracket = true;
      continue;
    }
    if (c == ']') {
      inBracket = false;
      continue;
    }
    if (!inBracket) {
      out.push_back(c);
    }
  }
  return out;
}

int pathDepth(const std::string& xpath) {
  int depth = 0;
  for (char c : xpath) {
    if (c == '/') {
      depth++;
    }
  }
  return depth;
}

// True if `prefix` is a proper ancestor path of `path` (prefix + "/" + ...).
bool isAncestorPath(const std::string& prefix, const std::string& path) {
  return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 && path[prefix.size()] == '/';
}

// ---- Stack tracking shared between forward and reverse ----

struct StackNode {
  std::string tag;
  int index = 1;
  bool hasText = false;
};

struct StackState {
  int skipDepth = -1;
  size_t totalTextBytes = 0;
  std::vector<StackNode> stack;
  std::vector<std::unordered_map<std::string, int>> siblingCounters;

  StackState() { siblingCounters.emplace_back(); }

  void pushElement(const XML_Char* rawName) {
    std::string name = toLowerStr(rawName ? rawName : "");
    const size_t depth = stack.size();
    if (siblingCounters.size() <= depth) {
      siblingCounters.resize(depth + 1);
    }
    const int sibIdx = ++siblingCounters[depth][name];
    stack.push_back({name, sibIdx, false});
    siblingCounters.emplace_back();
    if (skipDepth < 0 && isSkippableTag(name)) {
      skipDepth = static_cast<int>(stack.size()) - 1;
    }
  }

  void popElement() {
    if (stack.empty()) {
      return;
    }
    if (skipDepth == static_cast<int>(stack.size()) - 1) {
      skipDepth = -1;
    }
    stack.pop_back();
    if (!siblingCounters.empty()) {
      siblingCounters.pop_back();
    }
  }

  int bodyIdx() const {
    for (int i = static_cast<int>(stack.size()) - 1; i >= 0; i--) {
      if (stack[i].tag == "body") {
        return i;
      }
    }
    return -1;
  }

  bool insideBody() const { return bodyIdx() >= 0; }

  std::string currentXPath(const int spineIndex) const {
    const int bi = bodyIdx();
    std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
    if (bi < 0) {
      return xpath;
    }
    for (size_t i = static_cast<size_t>(bi + 1); i < stack.size(); i++) {
      xpath += "/" + stack[i].tag + "[" + std::to_string(stack[i].index) + "]";
    }
    return xpath;
  }

  bool shouldSkipText(const int len) const { return skipDepth >= 0 || len <= 0 || !insideBody(); }
};

// ---- Decompress spine item to temp file ----

std::string decompressToTempFile(const std::shared_ptr<Epub>& epub, const int spineIndex) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto spineItem = epub->getSpineItem(spineIndex);
  if (spineItem.href.empty()) {
    return "";
  }

  const std::string tmpPath = epub->getCachePath() + "/.tmp_kox.html";
  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }

  FsFile tmpFile;
  if (!Storage.openFileForWrite("KOX", tmpPath, tmpFile)) {
    LOG_ERR("KOX", "Failed to create temp file for spine=%d", spineIndex);
    return "";
  }

  constexpr size_t kChunkSize = 1024;
  const bool ok = epub->readItemContentsToStream(spineItem.href, tmpFile, kChunkSize);
  tmpFile.close();

  if (!ok) {
    Storage.remove(tmpPath.c_str());
    LOG_ERR("KOX", "Failed to decompress spine=%d to temp file", spineIndex);
    return "";
  }

  return tmpPath;
}

// ---- Expat parse loop ----
// Returns true on success or intentional stop (XML_ERROR_ABORTED).

bool runParse(XML_Parser parser, const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("KOX", path, file)) {
    return false;
  }

  constexpr size_t kBufSize = 1024;
  bool ok = true;
  int done;
  do {
    void* const buf = XML_GetBuffer(parser, kBufSize);
    if (!buf) {
      ok = false;
      break;
    }
    const size_t len = file.read(buf, kBufSize);
    done = file.available() == 0;
    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      ok = (XML_GetErrorCode(parser) == XML_ERROR_ABORTED);
      break;
    }
  } while (!done);

  file.close();
  return ok;
}

// ---- Entity reference filter (shared by all parse modes) ----

bool isEntityRef(const XML_Char* text, const int len) {
  if (len < 3 || text[0] != '&' || text[len - 1] != ';') {
    return false;
  }
  for (int i = 1; i < len - 1; ++i) {
    if (text[i] == '<' || text[i] == '>') {
      return false;
    }
  }
  return true;
}

// ============================================================
//  Pass 1 — Lightweight byte counter (no XPath string building)
// ============================================================

struct ByteCounter {
  int skipDepth = -1;
  int bodyStartDepth = -1;
  int depth = 0;
  size_t totalTextBytes = 0;
};

void XMLCALL bcStart(void* ud, const XML_Char* name, const XML_Char**) {
  auto* s = static_cast<ByteCounter*>(ud);
  const std::string tag = toLowerStr(name ? name : "");
  if (tag == "body" && s->bodyStartDepth < 0) {
    s->bodyStartDepth = s->depth;
  }
  if (s->skipDepth < 0 && isSkippableTag(tag)) {
    s->skipDepth = s->depth;
  }
  s->depth++;
}

void XMLCALL bcEnd(void* ud, const XML_Char*) {
  auto* s = static_cast<ByteCounter*>(ud);
  s->depth--;
  if (s->depth == s->skipDepth) {
    s->skipDepth = -1;
  }
  if (s->depth == s->bodyStartDepth) {
    s->bodyStartDepth = -1;
  }
}

void XMLCALL bcChar(void* ud, const XML_Char* text, const int len) {
  auto* s = static_cast<ByteCounter*>(ud);
  if (s->skipDepth >= 0 || s->bodyStartDepth < 0 || len <= 0 || isWhitespaceOnly(text, len)) {
    return;
  }
  s->totalTextBytes += countVisibleBytes(text, len);
}

void XMLCALL bcDefault(void* ud, const XML_Char* text, const int len) {
  if (isEntityRef(text, len)) {
    bcChar(ud, text, len);
  }
}

size_t countTotalTextBytes(const std::string& tmpPath) {
  ByteCounter state;
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    return 0;
  }
  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, bcStart, bcEnd);
  XML_SetCharacterDataHandler(parser, bcChar);
  XML_SetDefaultHandlerExpand(parser, bcDefault);
  runParse(parser, tmpPath);
  XML_ParserFree(parser);
  return state.totalTextBytes;
}

// ============================================================
//  Forward query: progress ratio → XPath (stop-early parse)
// ============================================================

struct ForwardState : StackState {
  int spineIndex;
  size_t targetOffset;
  std::string result;
  bool found = false;
  XML_Parser parser = nullptr;

  ForwardState(const int spineIndex, const size_t targetOffset) : spineIndex(spineIndex), targetOffset(targetOffset) {}

  void onChar(const XML_Char* text, const int len) {
    if (shouldSkipText(len) || isWhitespaceOnly(text, len) || found) {
      return;
    }

    const size_t visible = countVisibleBytes(text, len);
    if (totalTextBytes + visible >= targetOffset) {
      result = currentXPath(spineIndex);
      found = true;
      if (parser) {
        XML_StopParser(parser, XML_FALSE);
      }
      return;
    }
    totalTextBytes += visible;
  }
};

void XMLCALL fwdStart(void* ud, const XML_Char* name, const XML_Char**) {
  static_cast<ForwardState*>(ud)->pushElement(name);
}

void XMLCALL fwdEnd(void* ud, const XML_Char*) { static_cast<ForwardState*>(ud)->popElement(); }

void XMLCALL fwdChar(void* ud, const XML_Char* text, const int len) {
  static_cast<ForwardState*>(ud)->onChar(text, len);
}

void XMLCALL fwdDefault(void* ud, const XML_Char* text, const int len) {
  if (isEntityRef(text, len)) {
    fwdChar(ud, text, len);
  }
}

// ============================================================
//  Reverse query: XPath → progress ratio (full parse)
// ============================================================

enum class MatchTier : int {
  NONE = 0,
  ANCESTOR_NO_IDX = 1,
  ANCESTOR = 2,
  EXACT_NO_IDX = 3,
  EXACT = 4,
};

struct ReverseState : StackState {
  int spineIndex;
  std::string targetNorm;
  std::string targetNoIndex;

  MatchTier bestTier = MatchTier::NONE;
  int bestDepth = -1;
  size_t bestOffset = 0;
  bool bestExact = false;
  const char* bestTierName = nullptr;

  ReverseState(const int spineIndex, const std::string& xpath)
      : spineIndex(spineIndex), targetNorm(normalizeXPath(xpath)), targetNoIndex(removeIndices(targetNorm)) {}

  void onChar(const XML_Char* text, const int len) {
    if (shouldSkipText(len) || isWhitespaceOnly(text, len)) {
      return;
    }

    // Check match once per element (at first text).
    if (!stack.empty() && !stack.back().hasText) {
      stack.back().hasText = true;
      checkMatch();
    }

    totalTextBytes += countVisibleBytes(text, len);
  }

  void checkMatch() {
    // Normalize our generated XPath the same way as the target so that
    // "DocFragment" matches "docfragment".
    const std::string xpath = normalizeXPath(currentXPath(spineIndex));
    const int depth = pathDepth(xpath);

    if (xpath == targetNorm) {
      tryUpdate(MatchTier::EXACT, depth, "exact", true);
      return;
    }
    if (isAncestorPath(xpath, targetNorm)) {
      tryUpdate(MatchTier::ANCESTOR, depth, "ancestor", false);
      return;
    }

    const std::string xpathNoIdx = removeIndices(xpath);
    if (xpathNoIdx == targetNoIndex) {
      tryUpdate(MatchTier::EXACT_NO_IDX, depth, "index-insensitive", false);
    } else if (isAncestorPath(xpathNoIdx, targetNoIndex)) {
      tryUpdate(MatchTier::ANCESTOR_NO_IDX, depth, "index-insensitive-ancestor", false);
    }
  }

  void tryUpdate(const MatchTier tier, const int depth, const char* tierName, const bool isExact) {
    if (tier > bestTier || (tier == bestTier && depth > bestDepth)) {
      bestTier = tier;
      bestDepth = depth;
      bestOffset = totalTextBytes;
      bestExact = isExact;
      bestTierName = tierName;
    }
  }
};

void XMLCALL revStart(void* ud, const XML_Char* name, const XML_Char**) {
  static_cast<ReverseState*>(ud)->pushElement(name);
}

void XMLCALL revEnd(void* ud, const XML_Char*) { static_cast<ReverseState*>(ud)->popElement(); }

void XMLCALL revChar(void* ud, const XML_Char* text, const int len) {
  static_cast<ReverseState*>(ud)->onChar(text, len);
}

void XMLCALL revDefault(void* ud, const XML_Char* text, const int len) {
  if (isEntityRef(text, len)) {
    revChar(ud, text, len);
  }
}

}  // namespace

// ============================================================
//  Public API
// ============================================================

std::string ChapterXPathIndexer::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                      const float intraSpineProgress) {
  const std::string tmpPath = decompressToTempFile(epub, spineIndex);
  if (tmpPath.empty()) {
    return "";
  }

  // Pass 1: count total visible text bytes (lightweight, no XPath building).
  const size_t totalTextBytes = countTotalTextBytes(tmpPath);
  if (totalTextBytes == 0) {
    Storage.remove(tmpPath.c_str());
    const std::string base = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
    LOG_DBG("KOX", "Forward: spine=%d no text, returning base xpath", spineIndex);
    return base;
  }

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetOffset = static_cast<size_t>(clamped * static_cast<float>(totalTextBytes));

  // Pass 2: parse with full XPath tracking, stop as soon as target is reached.
  ForwardState state(spineIndex, targetOffset);
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    Storage.remove(tmpPath.c_str());
    return "";
  }
  state.parser = parser;
  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, fwdStart, fwdEnd);
  XML_SetCharacterDataHandler(parser, fwdChar);
  XML_SetDefaultHandlerExpand(parser, fwdDefault);
  runParse(parser, tmpPath);
  XML_ParserFree(parser);
  Storage.remove(tmpPath.c_str());

  if (state.result.empty()) {
    state.result = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  }

  LOG_DBG("KOX", "Forward: spine=%d progress=%.3f target=%zu/%zu -> %s", spineIndex, intraSpineProgress, targetOffset,
          totalTextBytes, state.result.c_str());
  return state.result;
}

bool ChapterXPathIndexer::findProgressForXPath(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                               const std::string& xpath, float& outIntraSpineProgress,
                                               bool& outExactMatch) {
  outIntraSpineProgress = 0.0f;
  outExactMatch = false;

  if (xpath.empty()) {
    return false;
  }

  const std::string tmpPath = decompressToTempFile(epub, spineIndex);
  if (tmpPath.empty()) {
    return false;
  }

  // Single pass: match target XPath inline, count totalTextBytes to end.
  ReverseState state(spineIndex, xpath);
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, revStart, revEnd);
  XML_SetCharacterDataHandler(parser, revChar);
  XML_SetDefaultHandlerExpand(parser, revDefault);
  const bool parseOk = runParse(parser, tmpPath);

  if (!parseOk) {
    LOG_ERR("KOX", "XPath parse failed for spine=%d at line %lu: %s", spineIndex, XML_GetCurrentLineNumber(parser),
            XML_ErrorString(XML_GetErrorCode(parser)));
  }
  XML_ParserFree(parser);
  Storage.remove(tmpPath.c_str());

  if (!parseOk || state.bestTier == MatchTier::NONE) {
    LOG_DBG("KOX", "Reverse: spine=%d no match for '%s'", spineIndex, xpath.c_str());
    return false;
  }

  outExactMatch = state.bestExact;
  if (state.totalTextBytes == 0) {
    outIntraSpineProgress = 0.0f;
  } else {
    outIntraSpineProgress = static_cast<float>(state.bestOffset) / static_cast<float>(state.totalTextBytes);
    outIntraSpineProgress = std::max(0.0f, std::min(1.0f, outIntraSpineProgress));
  }

  LOG_DBG("KOX", "Reverse: spine=%d %s match offset=%zu/%zu -> progress=%.3f for '%s'", spineIndex, state.bestTierName,
          state.bestOffset, state.totalTextBytes, outIntraSpineProgress, xpath.c_str());
  return true;
}

bool ChapterXPathIndexer::tryExtractSpineIndexFromXPath(const std::string& xpath, int& outSpineIndex) {
  outSpineIndex = -1;
  if (xpath.empty()) {
    return false;
  }

  const std::string normalized = normalizeXPath(xpath);
  const std::string key = "/docfragment[";
  const size_t pos = normalized.find(key);
  if (pos == std::string::npos) {
    LOG_DBG("KOX", "No DocFragment in xpath: '%s'", xpath.c_str());
    return false;
  }

  const size_t start = pos + key.size();
  size_t end = start;
  while (end < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[end]))) {
    end++;
  }

  if (end == start || end >= normalized.size() || normalized[end] != ']') {
    return false;
  }

  const std::string value = normalized.substr(start, end - start);
  const long parsed = std::strtol(value.c_str(), nullptr, 10);
  // KOReader uses 1-based DocFragment indices; convert to 0-based spine index.
  if (parsed < 1 || parsed > std::numeric_limits<int>::max()) {
    return false;
  }

  outSpineIndex = static_cast<int>(parsed) - 1;
  return true;
}

bool ChapterXPathIndexer::tryExtractParagraphIndexFromXPath(const std::string& xpath, uint16_t& outParagraphIndex) {
  outParagraphIndex = 0;
  if (xpath.empty()) {
    return false;
  }

  const std::string normalized = normalizeXPath(xpath);

  // Find /p[ after the second /body/ (the inner body inside DocFragment)
  const std::string bodyKey = "/body";
  size_t secondBody = normalized.find(bodyKey);
  if (secondBody != std::string::npos) {
    secondBody = normalized.find(bodyKey, secondBody + bodyKey.size());
  }

  const std::string pKey = "/p[";
  const size_t pos = normalized.find(pKey, secondBody != std::string::npos ? secondBody : 0);
  if (pos == std::string::npos) {
    return false;
  }

  const size_t start = pos + pKey.size();
  size_t end = start;
  while (end < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[end]))) {
    end++;
  }

  if (end == start || end >= normalized.size() || normalized[end] != ']') {
    return false;
  }

  const long parsed = std::strtol(normalized.substr(start, end - start).c_str(), nullptr, 10);
  if (parsed < 1 || parsed > UINT16_MAX) {
    return false;
  }

  outParagraphIndex = static_cast<uint16_t>(parsed);
  return true;
}
