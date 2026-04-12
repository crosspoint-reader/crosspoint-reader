#pragma once

#include <Epub.h>
#include <expat.h>

#include <memory>
#include <string>

namespace ChapterXPathIndexerInternal {

std::string toLowerStr(std::string value);

bool isSkippableTag(const std::string& tag);
bool isWhitespaceOnly(const XML_Char* text, int len);

size_t countVisibleBytes(const XML_Char* text, int len);
size_t countUtf8Codepoints(const XML_Char* text, int len);
size_t codepointAtVisibleByte(const XML_Char* text, int len, size_t targetVisibleByte);
size_t visibleBytesBeforeCodepoint(const XML_Char* text, int len, size_t targetCodepointOffset);

std::string normalizeXPath(const std::string& input);
std::string removeIndices(const std::string& xpath);
int pathDepth(const std::string& xpath);
bool isAncestorPath(const std::string& prefix, const std::string& path);

std::string decompressToTempFile(const std::shared_ptr<Epub>& epub, int spineIndex);
bool runParse(XML_Parser parser, const std::string& path);
bool isEntityRef(const XML_Char* text, int len);
size_t countTotalTextBytes(const std::string& tmpPath);

}  // namespace ChapterXPathIndexerInternal
