#include "ChapterXPathResolver.h"

#include <Epub/VisibleTextUtils.h>
#include <Logging.h>
#include <Memory.h>
#include <Print.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "Epub/htmlEntities.h"

namespace {
std::string stripPrefix(const XML_Char* name) {
  if (!name) {
    return "";
  }

  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

struct NameCounter {
  std::string name;
  int count;
};

struct ParentState {
  std::vector<NameCounter> children;

  int nextIndex(const std::string& name) {
    for (auto& child : children) {
      if (child.name == name) {
        child.count++;
        return child.count;
      }
    }

    children.push_back({name, 1});
    return 1;
  }
};

struct PathSegment {
  std::string name;
  int index;
};

std::string buildParagraphXPath(const int spineIndex, const std::vector<PathSegment>& path, const int textNodeIndex,
                                const size_t charOffset) {
  std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  for (const auto& segment : path) {
    xpath += "/" + segment.name + "[" + std::to_string(segment.index) + "]";
  }
  if (textNodeIndex > 0) {
    xpath += "/text()[" + std::to_string(textNodeIndex) + "]." + std::to_string(charOffset);
  }
  return xpath;
}

size_t countUtf8Codepoints(const XML_Char* data, const int len) {
  if (!data || len <= 0) {
    return 0;
  }

  size_t count = 0;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = ptr + len;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }

  return count;
}

class ParagraphTextCounter final : public Print {
 public:
  ParagraphTextCounter() {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &ParagraphTextCounter::startElement, &ParagraphTextCounter::endElement);
    XML_SetCharacterDataHandler(parser, &ParagraphTextCounter::characterData);
  }

  ~ParagraphTextCounter() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  size_t totalVisibleChars() const { return visibleChars; }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
      }
      depth++;
      return;
    }

    if (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name)) {
      nonVisibleDepth++;
    }
    if (name == "p") {
      paragraphDepth++;
    }
    if (name == "li") {
      liDepth++;
    }
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      nonVisibleDepth = 0;
      return;
    }

    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
    }
    if (name == "p" && paragraphDepth > 0) {
      paragraphDepth--;
    }
    if (name == "li" && liDepth > 0) {
      liDepth--;
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || (paragraphDepth <= 0 && liDepth <= 0) || len <= 0) {
      return;
    }

    visibleChars += countUtf8Codepoints(data, len);
  }

 private:
  XML_Parser parser = nullptr;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphDepth = 0;
  int liDepth = 0;
  uint16_t nonVisibleDepth = 0;
  size_t visibleChars = 0;
};

class XPathParagraphResolver final : public Print {
 public:
  explicit XPathParagraphResolver(const int targetParagraph) : targetParagraph(targetParagraph) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathParagraphResolver::startElement, &XPathParagraphResolver::endElement);
  }

  ~XPathParagraphResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onEndElement(name);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();

    // Count both <p> and <li> as paragraph-like positions, matching how the section
    // layout tracks them (xpathParagraphIndex and xpathListItemIndex). This ensures
    // KOReader progress in list items maps to the correct XPath.
    if (name == "p") {
      paragraphCount++;
    } else if (name == "li") {
      paragraphCount++;
    }
    if (paragraphCount == targetParagraph) {
      xpath = buildParagraphXPath(spineIndex, path, 0, 0);
      stopped = true;
      XML_StopParser(parser, XML_FALSE);
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      return;
    }

    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  XML_Parser parser = nullptr;
  const int targetParagraph;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphCount = 0;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};

struct TextNodeState {
  int index = 0;
  size_t startVisibleChars = 0;
  bool open = false;
};

class XPathProgressResolver final : public Print {
 public:
  enum class BoundaryMode { Exclusive, Inclusive };

  explicit XPathProgressResolver(const size_t targetVisibleChar,
                                 const BoundaryMode boundaryMode = BoundaryMode::Exclusive)
      : targetVisibleChar(targetVisibleChar), boundaryMode(boundaryMode) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    if (boundaryMode == BoundaryMode::Exclusive) {
      parentStates.reserve(MAX_DEPTH + 1);
      textNodeStates.reserve(MAX_DEPTH + 1);
      path.reserve(MAX_DEPTH);
    }
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathProgressResolver::startElement, &XPathProgressResolver::endElement);
    XML_SetCharacterDataHandler(parser, &XPathProgressResolver::characterData);
    if (boundaryMode == BoundaryMode::Exclusive) {
      XML_SetDefaultHandlerExpand(parser, &XPathProgressResolver::defaultHandlerExpand);
    }
    XML_SetCommentHandler(parser, &XPathProgressResolver::comment);
    XML_SetProcessingInstructionHandler(parser, &XPathProgressResolver::processingInstruction);
    XML_SetCdataSectionHandler(parser, &XPathProgressResolver::cdataBoundary, &XPathProgressResolver::cdataBoundary);
  }

  ~XPathProgressResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    if (parseOk && xpath.empty() && boundaryMode == BoundaryMode::Exclusive && targetVisibleChar == visibleChars) {
      xpath = std::move(boundaryXPath);
    }
    return parseOk;
  }

  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, const size_t size) override {
    if (!parser || !parseOk) {
      return size;
    }
    if (stopped) {
      return boundaryMode == BoundaryMode::Exclusive ? 0 : size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return stopped && boundaryMode == BoundaryMode::Exclusive ? 0 : size;
  }

  int spineIndex = 0;

 private:
  // Match the maximum ancestry depth understood by ProgressMapper.
  static constexpr size_t MAX_DEPTH = 16;

  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    static_cast<XPathProgressResolver*>(userData)->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    static_cast<XPathProgressResolver*>(userData)->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    static_cast<XPathProgressResolver*>(userData)->onCharacterData(data, len);
  }

  static void XMLCALL comment(void* userData, const XML_Char*) {
    static_cast<XPathProgressResolver*>(userData)->onMarkupBoundary();
  }

  static void XMLCALL processingInstruction(void* userData, const XML_Char*, const XML_Char*) {
    static_cast<XPathProgressResolver*>(userData)->onMarkupBoundary();
  }

  static void XMLCALL cdataBoundary(void* userData) {
    static_cast<XPathProgressResolver*>(userData)->onMarkupBoundary();
  }

  void stopWithoutMatch() {
    parseOk = false;
    stopped = true;
    XML_StopParser(parser, XML_FALSE);
  }

  void onMarkupBoundary() {
    if (!insideBody || nonVisibleDepth > 0 || stopped || textNodeStates.empty()) return;
    textNodeStates.back().open = false;
  }

  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* data, const int len) {
    if (len < 3 || data[0] != '&' || data[len - 1] != ';') {
      return;
    }

    auto* self = static_cast<XPathProgressResolver*>(userData);
    const char* resolved = lookupHtmlEntity(data, static_cast<size_t>(len));
    if (resolved) {
      self->onCharacterData(resolved, static_cast<int>(std::strlen(resolved)));
    } else {
      self->onCharacterData(data, len);
    }
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (VisibleTextUtils::equalsTag(name, "body")) {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
        textNodeStates.emplace_back();
      }
      depth++;
      return;
    }

    if (!textNodeStates.empty()) {
      textNodeStates.back().open = false;
    }
    if (boundaryMode == BoundaryMode::Exclusive && path.size() >= MAX_DEPTH) {
      LOG_DBG("KOX", "Exact XPath fallback: ancestry exceeds %zu elements", MAX_DEPTH);
      stopWithoutMatch();
      return;
    }
    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();
    textNodeStates.emplace_back();
    if (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name)) {
      nonVisibleDepth++;
    }
    if (name == "p") paragraphDepth++;
    if (name == "li") liDepth++;
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && VisibleTextUtils::equalsTag(name, "body")) {
      insideBody = false;
      parentStates.clear();
      textNodeStates.clear();
      path.clear();
      return;
    }

    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
    }
    if (name == "p" && paragraphDepth > 0) paragraphDepth--;
    if (name == "li" && liDepth > 0) liDepth--;
    if (!textNodeStates.empty()) {
      textNodeStates.pop_back();
    }
    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
    if (!textNodeStates.empty()) {
      textNodeStates.back().open = false;
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || len <= 0 || stopped || textNodeStates.empty()) {
      return;
    }

    if (boundaryMode == BoundaryMode::Inclusive && paragraphDepth <= 0 && liDepth <= 0) return;

    const size_t codepointCount = countUtf8Codepoints(data, len);
    if (codepointCount == 0) {
      return;
    }

    TextNodeState& textNode = textNodeStates.back();
    if (!textNode.open) {
      textNode.index++;
      textNode.startVisibleChars = visibleChars;
      textNode.open = true;
    }

    const size_t nextVisibleChars = visibleChars + codepointCount;
    const bool targetInCurrentChunk = boundaryMode == BoundaryMode::Inclusive ? targetVisibleChar <= nextVisibleChars
                                                                              : targetVisibleChar < nextVisibleChars;
    if (targetInCurrentChunk) {
      const size_t delta = targetVisibleChar - visibleChars;
      const size_t charOffset = visibleChars - textNode.startVisibleChars + delta;
      xpath = buildParagraphXPath(spineIndex, path, textNode.index, charOffset);
      stopped = true;
      XML_StopParser(parser, XML_FALSE);
      return;
    }
    if (targetVisibleChar == nextVisibleChars) {
      // Prefer the beginning of the following text node at an element/chunk
      // boundary. Retain this end-of-node anchor only for the document end.
      boundaryXPath =
          buildParagraphXPath(spineIndex, path, textNode.index, nextVisibleChars - textNode.startVisibleChars);
    }

    visibleChars = nextVisibleChars;
  }

  XML_Parser parser = nullptr;
  const size_t targetVisibleChar;
  const BoundaryMode boundaryMode;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphDepth = 0;
  int liDepth = 0;
  size_t nonVisibleDepth = 0;
  size_t visibleChars = 0;
  std::vector<TextNodeState> textNodeStates;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
  std::string boundaryXPath;
};
}  // namespace

std::string ChapterXPathResolver::findXPathForParagraph(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                        const uint16_t paragraphIndex) {
  if (!epub || paragraphIndex == 0 || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  XPathParagraphResolver resolver(paragraphIndex);
  if (!resolver.ok()) {
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved paragraph %u in spine %d -> %s", paragraphIndex, spineIndex, resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Paragraph %u not found in spine %d", paragraphIndex, spineIndex);
  return "";
}

std::string ChapterXPathResolver::findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                                const uint32_t visibleTextOffset) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    LOG_DBG("KOX", "Exact XPath skipped: invalid spine %d", spineIndex);
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    LOG_DBG("KOX", "Exact XPath skipped: empty href for spine %d", spineIndex);
    return "";
  }

  // Keep the parser and its per-depth state off the reader task's small stack.
  // One chapter is streamed once, stopping as soon as the target is resolved.
  auto resolver = makeUniqueNoThrow<XPathProgressResolver>(visibleTextOffset);
  if (!resolver) {
    LOG_ERR("KOX", "OOM: visible-offset XPath resolver");
    return "";
  }
  if (!resolver->ok()) {
    return "";
  }

  resolver->spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, *resolver, 1024, true) || !resolver->finish()) {
    return "";
  }
  return resolver->hasMatch() ? resolver->getXPath() : "";
}

std::string ChapterXPathResolver::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const float intraSpineProgress) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  if (!(intraSpineProgress > 0.0f)) {
    return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  }

  ParagraphTextCounter counter;
  if (!counter.ok() || !epub->readItemContentsToStream(href, counter, 1024) || !counter.finish()) {
    return "";
  }

  const size_t totalVisibleChars = counter.totalVisibleChars();
  if (totalVisibleChars == 0) {
    return "";
  }

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetVisibleChar =
      std::max<size_t>(1, std::min(totalVisibleChars, static_cast<size_t>(std::ceil(clamped * totalVisibleChars))));

  XPathProgressResolver resolver(targetVisibleChar, XPathProgressResolver::BoundaryMode::Inclusive);
  if (!resolver.ok()) {
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved progress %.3f in spine %d -> %s", intraSpineProgress, spineIndex,
            resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Could not resolve progress %.3f in spine %d", intraSpineProgress, spineIndex);
  return "";
}
