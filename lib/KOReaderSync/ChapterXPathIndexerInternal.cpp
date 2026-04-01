#include "ChapterXPathIndexerInternal.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <vector>

namespace ChapterXPathIndexerInternal {

std::string toLowerStr(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

size_t countUtf8Codepoints(const XML_Char* text, const int len) {
  size_t count = 0;
  for (int i = 0; i < len; i++) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
      count++;
    }
  }
  return count;
}

size_t codepointAtVisibleByte(const XML_Char* text, const int len, const size_t targetVisibleByte) {
  size_t codepoints = 0;
  size_t visibleBytes = 0;
  for (int i = 0; i < len; i++) {
    const unsigned char uc = static_cast<unsigned char>(text[i]);
    const bool isLeadByte = (uc & 0xC0) != 0x80;
    if (isLeadByte) {
      codepoints++;
    }
    if (!std::isspace(uc)) {
      if (visibleBytes == targetVisibleByte) {
        return codepoints - 1;
      }
      visibleBytes++;
    }
  }
  return codepoints;
}

size_t visibleBytesBeforeCodepoint(const XML_Char* text, const int len, const size_t targetCodepointOffset) {
  size_t visibleBytes = 0;
  size_t codepointIndex = 0;

  int i = 0;
  while (i < len) {
    if (codepointIndex >= targetCodepointOffset) {
      break;
    }

    const int cpStart = i;
    i++;
    while (i < len && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
      i++;
    }

    for (int j = cpStart; j < i; j++) {
      if (!std::isspace(static_cast<unsigned char>(text[j]))) {
        visibleBytes++;
      }
    }

    codepointIndex++;
  }

  return visibleBytes;
}

std::string normalizeXPath(const std::string& input) {
  if (input.empty()) {
    return "";
  }

  std::string out;
  out.reserve(input.size());
  for (const char c : input) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isspace(uc)) {
      continue;
    }
    out.push_back(static_cast<char>(std::tolower(uc)));
  }

  const std::string textTag = "/text()";
  const size_t textPos = out.rfind(textTag);
  if (textPos != std::string::npos) {
    const size_t afterText = textPos + textTag.size();
    if (afterText == out.size() || out[afterText] == '.' || out[afterText] == '[') {
      out.erase(textPos);
    }
  }

  const size_t lastSlash = out.rfind('/');
  if (lastSlash != std::string::npos) {
    const size_t dotPos = out.find('.', lastSlash + 1);
    if (dotPos != std::string::npos && dotPos + 1 < out.size()) {
      bool allDigits = true;
      for (size_t i = dotPos + 1; i < out.size(); i++) {
        if (!std::isdigit(static_cast<unsigned char>(out[i]))) {
          allDigits = false;
          break;
        }
      }
      if (allDigits) {
        out.erase(dotPos);
      }
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
  for (const char c : xpath) {
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
  for (const char c : xpath) {
    if (c == '/') {
      depth++;
    }
  }
  return depth;
}

bool isAncestorPath(const std::string& prefix, const std::string& path) {
  return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 && path[prefix.size()] == '/';
}

std::string decompressToTempFile(const std::shared_ptr<Epub>& epub, const int spineIndex) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto spineItem = epub->getSpineItem(spineIndex);
  if (spineItem.href.empty()) {
    return "";
  }

  const std::string tmpPath = epub->getCachePath() + "/.tmp_kox_" + std::to_string(spineIndex) + ".html";
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

namespace {

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

}  // namespace

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

}  // namespace ChapterXPathIndexerInternal
