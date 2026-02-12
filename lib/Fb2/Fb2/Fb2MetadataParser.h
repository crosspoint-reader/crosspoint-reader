#pragma once

#include <string>
#include <vector>

#include "Fb2.h"

// Streaming expat-based parser for FB2 metadata.
// First pass: extracts title, author, language, cover ID from <description>,
// and scans <body> to identify section boundaries and titles with file offsets.
class Fb2MetadataParser {
  std::string filepath;

  // Metadata
  std::string title;
  std::string author;
  std::string language;
  std::string coverBinaryId;

  // Section scanning
  std::vector<Fb2::SectionInfo> sections;
  std::vector<Fb2::TocEntry> tocEntries;

  // Parser state
  enum class Context {
    NONE,
    TITLE_INFO,
    BOOK_TITLE,
    AUTHOR_FIRST_NAME,
    AUTHOR_MIDDLE_NAME,
    AUTHOR_LAST_NAME,
    LANG,
    COVERPAGE,
    BODY,
    SECTION_TITLE,
    SECTION_TITLE_P,
    BINARY_SCAN
  };
  Context context = Context::NONE;
  int bodyDepth = 0;
  int sectionDepth = 0;
  bool inBody = false;
  bool inTitleInfo = false;
  bool inAuthor = false;
  std::string charBuffer;
  std::string authorFirstName;
  std::string authorMiddleName;
  std::string authorLastName;
  std::string currentSectionTitle;
  bool inSectionTitle = false;
  size_t currentSectionOffset = 0;
  size_t previousSectionEnd = 0;
  int currentSectionIndex = 0;

  // Track byte offset from expat
  void* parser = nullptr;

  static void startElement(void* userData, const char* name, const char** atts);
  static void endElement(void* userData, const char* name);
  static void characterData(void* userData, const char* s, int len);

 public:
  explicit Fb2MetadataParser(const std::string& filepath) : filepath(filepath) {}
  bool parse();

  const std::string& getTitle() const { return title; }
  const std::string& getAuthor() const { return author; }
  const std::string& getLanguage() const { return language; }
  const std::string& getCoverBinaryId() const { return coverBinaryId; }
  const std::vector<Fb2::SectionInfo>& getSections() const { return sections; }
  const std::vector<Fb2::TocEntry>& getTocEntries() const { return tocEntries; }
};
