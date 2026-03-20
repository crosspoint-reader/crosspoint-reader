#include "ChapterXPathIndexer.h"

#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Anchor used for both mapping directions.
// textOffset is counted as visible (non-whitespace) bytes from chapter start.
// xpath points to the nearest element path at/near that offset.

struct XPathAnchor {
  size_t textOffset = 0;
  std::string xpath;
  std::string xpathNoIndex;  // precomputed removeIndices(xpath)
};

struct StackNode {
  std::string tag;
  int index = 1;
  bool hasTextAnchor = false;
};

// ParserState is intentionally ephemeral and created per lookup call.
// It holds only one spine parse worth of data to avoid retaining structures
// that would increase long-lived heap usage on the ESP32-C3.
struct ParserState {
  explicit ParserState(const int spineIndex) : spineIndex(spineIndex) { siblingCounters.emplace_back(); }

  int spineIndex = 0;
  int skipDepth = -1;
  size_t totalTextBytes = 0;

  std::vector<StackNode> stack;
  std::vector<std::unordered_map<std::string, int>> siblingCounters;
  std::vector<XPathAnchor> anchors;

  std::string baseXPath() const { return "/body/docfragment[" + std::to_string(spineIndex + 1) + "]/body"; }

  // Canonicalize incoming KOReader XPath before matching:
  // - remove all whitespace
  // - lowercase tags
  // - strip optional trailing /text()
  // - strip trailing slash
  static std::string normalizeXPath(const std::string& input) {
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

    const std::string textSuffix = "/text()";
    const size_t textPos = out.rfind(textSuffix);
    if (textPos != std::string::npos && textPos + textSuffix.size() == out.size()) {
      out.erase(textPos);
    }

    while (!out.empty() && out.back() == '/') {
      out.pop_back();
    }

    return out;
  }

  // Remove bracketed numeric predicates so paths can be compared even when
  // index counters differ between parser implementations.
  static std::string removeIndices(const std::string& xpath) {
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

  static int pathDepth(const std::string& xpath) {
    int depth = 0;
    for (char c : xpath) {
      if (c == '/') {
        depth++;
      }
    }
    return depth;
  }

  // Resolve a path to the best anchor offset.
  // If exact node path is not found, progressively trim trailing segments and
  // match ancestors to obtain a stable approximate location.
  bool pickBestAnchorByPath(const std::string& targetPath, const bool ignoreIndices, size_t& outTextOffset,
                            bool& outExact) const {
    if (targetPath.empty() || anchors.empty()) {
      return false;
    }

    const std::string normalizedTarget = ignoreIndices ? removeIndices(targetPath) : targetPath;
    std::string probe = normalizedTarget;
    bool exactProbe = true;

    while (!probe.empty()) {
      int bestDepth = -1;
      size_t bestOffset = 0;
      bool found = false;

      for (const auto& anchor : anchors) {
        const std::string& anchorPath = ignoreIndices ? anchor.xpathNoIndex : anchor.xpath;
        if (anchorPath == probe) {
          const int depth = pathDepth(anchorPath);
          if (!found || depth > bestDepth || (depth == bestDepth && anchor.textOffset > bestOffset)) {
            found = true;
            bestDepth = depth;
            bestOffset = anchor.textOffset;
          }
        }
      }

      if (found) {
        outTextOffset = bestOffset;
        outExact = exactProbe;
        return true;
      }

      const size_t lastSlash = probe.find_last_of('/');
      if (lastSlash == std::string::npos || lastSlash == 0) {
        break;
      }
      probe.erase(lastSlash);
      exactProbe = false;
    }

    return false;
  }

  static std::string toLower(std::string value) {
    for (char& c : value) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
  }

  // Elements that should not contribute text position anchors.
  static bool isSkippableTag(const std::string& tag) { return tag == "head" || tag == "script" || tag == "style"; }

  static bool isWhitespaceOnly(const XML_Char* text, const int len) {
    for (int i = 0; i < len; i++) {
      if (!std::isspace(static_cast<unsigned char>(text[i]))) {
        return false;
      }
    }
    return true;
  }

  // Count non-whitespace bytes to keep offsets stable against formatting-only
  // differences and indentation in source XHTML.
  static size_t countVisibleBytes(const XML_Char* text, const int len) {
    size_t count = 0;
    for (int i = 0; i < len; i++) {
      if (!std::isspace(static_cast<unsigned char>(text[i]))) {
        count++;
      }
    }
    return count;
  }

  int bodyDepth() const {
    for (int i = static_cast<int>(stack.size()) - 1; i >= 0; i--) {
      if (stack[i].tag == "body") {
        return i;
      }
    }
    return -1;
  }

  bool insideBody() const { return bodyDepth() >= 0; }

  std::string currentXPath() const {
    const int bodyIdx = bodyDepth();
    if (bodyIdx < 0) {
      return baseXPath();
    }

    std::string xpath = baseXPath();
    for (size_t i = static_cast<size_t>(bodyIdx + 1); i < stack.size(); i++) {
      xpath += "/" + stack[i].tag + "[" + std::to_string(stack[i].index) + "]";
    }
    return xpath;
  }

  // Adds first anchor for an element when text begins and periodic anchors in
  // longer runs so matching has sufficient granularity without exploding memory.
  void addAnchorIfNeeded() {
    if (!insideBody() || stack.empty()) {
      return;
    }

    if (!stack.back().hasTextAnchor) {
      const std::string xpath = currentXPath();
      anchors.push_back({totalTextBytes, xpath, removeIndices(xpath)});
      stack.back().hasTextAnchor = true;
    } else if (anchors.empty() || totalTextBytes - anchors.back().textOffset >= 192) {
      const std::string xpath = currentXPath();
      if (anchors.empty() || anchors.back().xpath != xpath) {
        anchors.push_back({totalTextBytes, xpath, removeIndices(xpath)});
      }
    }
  }

  void onStartElement(const XML_Char* rawName) {
    std::string name = toLower(rawName ? rawName : "");
    const size_t depth = stack.size();

    if (siblingCounters.size() <= depth) {
      siblingCounters.resize(depth + 1);
    }
    const int siblingIndex = ++siblingCounters[depth][name];

    stack.push_back({name, siblingIndex, false});
    siblingCounters.emplace_back();

    if (skipDepth < 0 && isSkippableTag(name)) {
      skipDepth = static_cast<int>(stack.size()) - 1;
    }
  }

  void onEndElement() {
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

  void onCharacterData(const XML_Char* text, const int len) {
    if (skipDepth >= 0 || len <= 0 || !insideBody() || isWhitespaceOnly(text, len)) {
      return;
    }

    addAnchorIfNeeded();
    totalTextBytes += countVisibleBytes(text, len);
  }

  std::string chooseXPath(const float intraSpineProgress) const {
    if (anchors.empty()) {
      return baseXPath();
    }
    if (totalTextBytes == 0) {
      return anchors.front().xpath;
    }

    const float clampedProgress = std::max(0.0f, std::min(1.0f, intraSpineProgress));
    const size_t target = static_cast<size_t>(clampedProgress * static_cast<float>(totalTextBytes));

    // upper_bound returns the first anchor strictly after target; step back to get
    // the last anchor at-or-before target (the element the user is currently inside).
    auto it = std::upper_bound(anchors.begin(), anchors.end(), target,
                               [](const size_t value, const XPathAnchor& anchor) { return value < anchor.textOffset; });
    if (it != anchors.begin()) {
      --it;
    }
    return it->xpath;
  }

  // Convert path -> progress ratio by matching to nearest available anchor.
  bool chooseProgressForXPath(const std::string& xpath, float& outIntraSpineProgress, bool& outExactMatch) const {
    if (anchors.empty()) {
      return false;
    }

    const std::string normalized = normalizeXPath(xpath);
    if (normalized.empty()) {
      return false;
    }

    size_t matchedOffset = 0;
    bool exact = false;
    const char* matchTier = nullptr;

    bool matched = pickBestAnchorByPath(normalized, false, matchedOffset, exact);
    if (matched) {
      matchTier = exact ? "exact" : "ancestor";
    } else {
      bool exactRaw = false;
      matched = pickBestAnchorByPath(normalized, true, matchedOffset, exactRaw);
      if (matched) {
        exact = false;
        matchTier = exactRaw ? "index-insensitive" : "index-insensitive-ancestor";
      }
    }

    if (!matched) {
      LOG_DBG("KOX", "Reverse: spine=%d no anchor match for '%s' (%zu anchors)", spineIndex, normalized.c_str(),
              anchors.size());
      return false;
    }

    outExactMatch = exact;
    if (totalTextBytes == 0) {
      outIntraSpineProgress = 0.0f;
      LOG_DBG("KOX", "Reverse: spine=%d %s match offset=%zu -> progress=0.0 (no text)", spineIndex, matchTier,
              matchedOffset);
      return true;
    }

    outIntraSpineProgress = static_cast<float>(matchedOffset) / static_cast<float>(totalTextBytes);
    outIntraSpineProgress = std::max(0.0f, std::min(1.0f, outIntraSpineProgress));
    LOG_DBG("KOX", "Reverse: spine=%d %s match offset=%zu/%zu -> progress=%.3f", spineIndex, matchTier, matchedOffset,
            totalTextBytes, outIntraSpineProgress);
    return true;
  }
};

void XMLCALL onStartElement(void* userData, const XML_Char* name, const XML_Char**) {
  auto* state = static_cast<ParserState*>(userData);
  state->onStartElement(name);
}

void XMLCALL onEndElement(void* userData, const XML_Char*) {
  auto* state = static_cast<ParserState*>(userData);
  state->onEndElement();
}

void XMLCALL onCharacterData(void* userData, const XML_Char* text, const int len) {
  auto* state = static_cast<ParserState*>(userData);
  state->onCharacterData(text, len);
}

void XMLCALL onDefaultHandlerExpand(void* userData, const XML_Char* text, const int len) {
  // The default handler fires for comments, PIs, DOCTYPE, and entity references.
  // Only forward entity references (&..;) to avoid skewing text offsets with
  // non-visible markup.
  if (len < 3 || text[0] != '&' || text[len - 1] != ';') {
    return;
  }
  for (int i = 1; i < len - 1; ++i) {
    if (text[i] == '<' || text[i] == '>') {
      return;
    }
  }
  auto* state = static_cast<ParserState*>(userData);
  state->onCharacterData(text, len);
}

// Parse one spine item and return a fully populated ParserState.
// Returns std::nullopt if validation, I/O, or XML parse fails.
static std::optional<ParserState> parseSpineItem(const std::shared_ptr<Epub>& epub, const int spineIndex) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return std::nullopt;
  }

  const auto spineItem = epub->getSpineItem(spineIndex);
  if (spineItem.href.empty()) {
    return std::nullopt;
  }

  // Phase 1: decompress EPUB entry to a temp file on SD.
  // This keeps the ~1.3 KB uzlib_uncomp struct off the stack before Expat runs.
  const std::string tmpPath = epub->getCachePath() + "/.tmp_kox.html";
  {
    if (Storage.exists(tmpPath.c_str())) {
      Storage.remove(tmpPath.c_str());
    }
    FsFile tmpFile;
    if (!Storage.openFileForWrite("KOX", tmpPath, tmpFile)) {
      LOG_ERR("KOX", "Failed to create temp file for spine=%d", spineIndex);
      return std::nullopt;
    }
    constexpr size_t kChunkSize = 1024;
    const bool ok = epub->readItemContentsToStream(spineItem.href, tmpFile, kChunkSize);
    tmpFile.close();
    if (!ok) {
      Storage.remove(tmpPath.c_str());
      LOG_ERR("KOX", "Failed to decompress spine=%d to temp file", spineIndex);
      return std::nullopt;
    }
  }

  // Phase 2: parse the temp file with Expat in chunks.
  ParserState state(spineIndex);

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    Storage.remove(tmpPath.c_str());
    LOG_ERR("KOX", "Failed to allocate XML parser for spine=%d", spineIndex);
    return std::nullopt;
  }

  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, onStartElement, onEndElement);
  XML_SetCharacterDataHandler(parser, onCharacterData);
  XML_SetDefaultHandlerExpand(parser, onDefaultHandlerExpand);

  FsFile tmpFile;
  bool parseOk = Storage.openFileForRead("KOX", tmpPath, tmpFile);

  if (parseOk) {
    constexpr size_t kParseBufSize = 1024;
    int done;
    do {
      void* const buf = XML_GetBuffer(parser, kParseBufSize);
      if (!buf) {
        parseOk = false;
        break;
      }
      const size_t len = tmpFile.read(buf, kParseBufSize);
      done = tmpFile.available() == 0;
      if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
        parseOk = false;
        break;
      }
    } while (!done);
    tmpFile.close();
  }

  Storage.remove(tmpPath.c_str());

  if (!parseOk) {
    LOG_ERR("KOX", "XPath parse failed for spine=%d at line %lu: %s", spineIndex, XML_GetCurrentLineNumber(parser),
            XML_ErrorString(XML_GetErrorCode(parser)));
  }

  XML_ParserFree(parser);

  if (!parseOk) {
    return std::nullopt;
  }

  return state;
}

// Keep a single parsed spine in memory to avoid repeated decompress+parse work
// in the KO sync flow.
//
// Why this helps here:
// - Sync result screen calls both mapping directions (remote->local for compare,
//   local->KOReader for display).
// - If the user chooses Upload, local->KOReader mapping is performed again.
// - Without caching, the same chapter can be decompressed and reparsed multiple
//   times during one activity session.
//
// Why the cache is exactly one entry:
// - We only need to accelerate immediate repeat lookups for the current chapter.
// - A single-entry cache bounds RAM use and avoids long-lived heap growth on
//   ESP32-C3.
static const ParserState* getParsedStateCached(const std::shared_ptr<Epub>& epub, const int spineIndex) {
  static const Epub* cachedEpub = nullptr;
  static int cachedSpineIndex = -1;
  static std::string cachedHref;
  static std::optional<ParserState> cachedState;

  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return nullptr;
  }

  const auto spineItem = epub->getSpineItem(spineIndex);
  if (spineItem.href.empty()) {
    return nullptr;
  }

  const bool cacheHit = cachedState.has_value() && cachedEpub == epub.get() && cachedSpineIndex == spineIndex &&
                        cachedHref == spineItem.href;
  if (cacheHit) {
    LOG_DBG("KOX", "Cache hit: spine=%d anchors=%zu textBytes=%zu", spineIndex, cachedState->anchors.size(),
            cachedState->totalTextBytes);
    return &cachedState.value();
  }

  auto parsed = parseSpineItem(epub, spineIndex);
  if (!parsed) {
    return nullptr;
  }

  cachedEpub = epub.get();
  cachedSpineIndex = spineIndex;
  cachedHref = spineItem.href;
  cachedState = std::move(parsed);

  LOG_DBG("KOX", "Cache store: spine=%d anchors=%zu textBytes=%zu", spineIndex, cachedState->anchors.size(),
          cachedState->totalTextBytes);
  return &cachedState.value();
}

}  // namespace

std::string ChapterXPathIndexer::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                      const float intraSpineProgress) {
  const auto* state = getParsedStateCached(epub, spineIndex);
  if (!state) {
    return "";
  }

  const std::string result = state->chooseXPath(intraSpineProgress);
  LOG_DBG("KOX", "Forward: spine=%d progress=%.3f anchors=%zu textBytes=%zu -> %s", spineIndex, intraSpineProgress,
          state->anchors.size(), state->totalTextBytes, result.c_str());
  return result;
}

bool ChapterXPathIndexer::findProgressForXPath(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                               const std::string& xpath, float& outIntraSpineProgress,
                                               bool& outExactMatch) {
  outIntraSpineProgress = 0.0f;
  outExactMatch = false;

  if (xpath.empty()) {
    return false;
  }

  const auto* state = getParsedStateCached(epub, spineIndex);
  if (!state) {
    return false;
  }

  LOG_DBG("KOX", "Reverse: spine=%d anchors=%zu textBytes=%zu for '%s'", spineIndex, state->anchors.size(),
          state->totalTextBytes, xpath.c_str());
  return state->chooseProgressForXPath(xpath, outIntraSpineProgress, outExactMatch);
}

bool ChapterXPathIndexer::tryExtractSpineIndexFromXPath(const std::string& xpath, int& outSpineIndex) {
  outSpineIndex = -1;
  if (xpath.empty()) {
    return false;
  }

  const std::string normalized = ParserState::normalizeXPath(xpath);
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
