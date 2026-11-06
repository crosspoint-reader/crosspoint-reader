#pragma once

// FB2 parsing passes, separated from the writer so host tests can exercise
// them without Storage/Arduino: pass 1 (description metadata) and pass 2
// (chapter/id/binary structure). Pure expat; see Fb2Converter.cpp for the
// emission pass that consumes these results.

#include <expat.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fb2passes {

struct Metadata {
  std::string title;
  std::string author;
  std::string language;
  std::string seriesName;
  std::string seriesNumber;  // raw textual position, "" when absent
  std::string coverRef;      // binary id of the coverpage image, "" if none
};

// Worst-case RAM: idTargets 1024 * ~52B = ~53 KB, materialized 1000 * ~104B
// = ~104 KB, both transient during conversion. The C3 baseline has ~380 KB
// heap, so these caps bound a pathological book (thousands of note anchors or
// chapters) to a graceful partial conversion instead of an OOM.
constexpr size_t kMaxIdTargets = 1024;
constexpr size_t kMaxBinaries = 512;
constexpr int kMaxChapters = 1000;

constexpr size_t kMaxTitleLen = 160;

inline const char* imageExtensionForContentType(const char* contentType) {
  if (!contentType) return "";
  if (strstr(contentType, "jpeg") || strstr(contentType, "jpg")) return ".jpg";
  if (strstr(contentType, "png")) return ".png";
  if (strstr(contentType, "gif")) return ".gif";
  return "";
}

inline const char* localName(const char* n) {
  const char* c = strchr(n, ':');
  return c ? c + 1 : n;
}

// Attribute lookup that tolerates namespace prefixes (l:href, xlink:href, href).
inline const char* findAttr(const char** attrs, const char* name) {
  for (int i = 0; attrs[i]; i += 2) {
    if (strcmp(localName(attrs[i]), name) == 0) return attrs[i + 1];
  }
  return nullptr;
}

struct TextCapture {
  char buf[kMaxTitleLen + 1] = {};
  size_t len = 0;
  bool active = false;
  bool taken = false;  // keep only the first occurrence

  void start() {
    active = true;
    if (!taken) len = 0;
  }
  void stop(bool keep) {
    active = false;
    if (keep && !taken && len > 0) taken = true;
  }
  void feed(const char* s, int n) {
    if (!active || taken) return;
    for (int i = 0; i < n && len < kMaxTitleLen; i++) {
      buf[len++] = s[i];
    }
    buf[len] = 0;
  }
  const char* text() const { return buf; }
};

// ---------------------------------------------------------------- pass 1 ---
struct MetaPass {
  Metadata meta;
  XML_Parser parser = nullptr;  // set by readMetadata to allow early stop
  int depth = 0;
  int inTitleInfo = 0;
  int inAuthor = 0;
  int inCoverpage = 0;
  bool seenBody = false;
  TextCapture bookTitle;
  TextCapture firstName;
  TextCapture lastName;
  TextCapture nickname;
  TextCapture lang;

  // Assembles the final metadata once the parse is done: direct fields were
  // filled during the parse, captured text moves over when taken. Shared by
  // the converter's readMetadata() and the host tests.
  Metadata collect() const {
    Metadata out = meta;
    if (bookTitle.taken) out.title = bookTitle.text();
    if (lang.taken) out.language = lang.text();
    return out;
  }
};

inline void XMLCALL metaStart(void* userData, const XML_Char* name, const char** attrs) {
  MetaPass& m = *static_cast<MetaPass*>(userData);
  const char* ln = localName(name);
  m.depth++;
  if (strcmp(ln, "body") == 0) {
    // Bodies carry no metadata: stop reading the file here.
    m.seenBody = true;
    if (m.parser) XML_StopParser(m.parser, XML_FALSE);
    return;
  }
  if (m.seenBody) return;
  if (m.inTitleInfo > 0) {
    if (strcmp(ln, "author") == 0) {
      m.inAuthor = m.depth;  // remember the author's own depth
      return;
    }
    if (m.inAuthor > 0) {
      if (strcmp(ln, "first-name") == 0) m.firstName.start();
      else if (strcmp(ln, "last-name") == 0) m.lastName.start();
      else if (strcmp(ln, "nickname") == 0) m.nickname.start();
      return;
    }
    if (strcmp(ln, "book-title") == 0 && !m.bookTitle.taken) m.bookTitle.start();
    else if (strcmp(ln, "lang") == 0) m.lang.start();
    else if (strcmp(ln, "sequence") == 0) {
      const char* nm = findAttr(attrs, "name");
      const char* num = findAttr(attrs, "number");
      if (nm && m.meta.seriesName.empty()) m.meta.seriesName = nm;
      if (num && m.meta.seriesNumber.empty()) m.meta.seriesNumber = num;
    } else if (strcmp(ln, "coverpage") == 0) {
      m.inCoverpage++;
    } else if (m.inCoverpage > 0 && strcmp(ln, "image") == 0 && m.meta.coverRef.empty()) {
      const char* href = findAttr(attrs, "href");
      if (href && href[0] == '#') m.meta.coverRef = href + 1;
    }
  } else if (strcmp(ln, "title-info") == 0) {
    m.inTitleInfo = m.depth;
  }
}

inline void XMLCALL metaEnd(void* userData, const XML_Char* name) {
  MetaPass& m = *static_cast<MetaPass*>(userData);
  const char* ln = localName(name);
  const int d = m.depth;
  m.depth--;  // element closes: decrement before any early exit below

  if (m.inTitleInfo == d && strcmp(ln, "title-info") == 0) {
    m.inTitleInfo = 0;
    return;
  }
  if (m.inTitleInfo <= 0) return;

  if (strcmp(ln, "author") == 0) {
    if (m.inAuthor == d && m.meta.author.empty()) {
      // Compose "First Last"; fall back to the nickname.
      std::string composed;
      if (m.firstName.taken) composed = m.firstName.text();
      if (m.lastName.taken) {
        if (!composed.empty()) composed += ' ';
        composed += m.lastName.text();
      }
      if (composed.empty() && m.nickname.taken) composed = m.nickname.text();
      m.meta.author = composed;
    }
    m.inAuthor = 0;  // depth marker, not a counter: leaving the author element clears it
    return;
  }
  if (m.inAuthor > 0) {
    if (strcmp(ln, "first-name") == 0) m.firstName.stop(true);
    else if (strcmp(ln, "last-name") == 0) m.lastName.stop(true);
    else if (strcmp(ln, "nickname") == 0) m.nickname.stop(true);
    return;
  }
  if (strcmp(ln, "book-title") == 0) m.bookTitle.stop(true);
  else if (strcmp(ln, "lang") == 0) m.lang.stop(true);
  else if (strcmp(ln, "coverpage") == 0 && m.inCoverpage > 0) m.inCoverpage--;
}

inline void XMLCALL metaText(void* userData, const XML_Char* s, int len) {
  MetaPass& m = *static_cast<MetaPass*>(userData);
  if (m.seenBody || m.inTitleInfo == 0) return;
  if (m.inAuthor > 0) {
    m.firstName.feed(s, len);
    m.lastName.feed(s, len);
    m.nickname.feed(s, len);
    return;
  }
  m.bookTitle.feed(s, len);
  m.lang.feed(s, len);
}

// ------------------------------------------------------------- structure ---
struct BinaryInfo {
  char id[48] = {};  // XML ids in real books are short; longer ones truncate
  char ext[8] = {};  // ".jpg" | ".png" | ".gif" | "" (unsupported)
};

struct IdTarget {
  char id[48] = {};
  int32_t chapter = 0;
};

struct StructurePass {
  int depth = 0;
  int bodyDepth = -1;    // depth of the current <body>
  int bodyCount = 0;
  int chapterCursor = -1;
  int sectionNesting = 0;  // open <section> elements inside the current body
  bool inBody = false;
  bool sawRoot = false;
  char rootName[32] = {};
  std::vector<BinaryInfo> binaries;
  std::vector<IdTarget> idTargets;

  bool recordId(const char* id) {
    if (!id || !id[0] || idTargets.size() >= kMaxIdTargets) return false;
    if (findIdChapter(id) >= 0) return false;  // first definition wins
    IdTarget& t = idTargets.emplace_back();
    strncpy(t.id, id, sizeof(t.id) - 1);
    t.chapter = chapterCursor;
    return true;
  }

  int32_t findIdChapter(const char* id) const {
    for (const IdTarget& t : idTargets) {
      if (strcmp(t.id, id) == 0) return t.chapter;
    }
    return -1;
  }

  int findBinary(const char* id) const {
    for (size_t i = 0; i < binaries.size(); i++) {
      if (strcmp(binaries[i].id, id) == 0) return (int)i;
    }
    return -1;
  }
};

inline void structureElement(StructurePass& s, const char* ln, const char** attrs, bool start) {
  if (strcmp(ln, "body") == 0) {
    if (start) {
      s.bodyCount++;
      s.bodyDepth = s.depth;  // depth already incremented for start
      s.inBody = true;
      s.sectionNesting = 0;
      // Every body gets a chapter slot for its front matter (title/epigraph
      // before the sections); the entry opens lazily, so empty slots cost
      // nothing. Must mirror the emission pass exactly.
      s.chapterCursor++;
    } else {
      s.inBody = false;
      s.bodyDepth = -1;
    }
    return;
  }
  if (!s.inBody) {
    if (start && strcmp(ln, "binary") == 0) {
      const char* id = findAttr(attrs, "id");
      const char* ct = findAttr(attrs, "content-type");
      if (id && ct && s.binaries.size() < kMaxBinaries) {
        BinaryInfo& b = s.binaries.emplace_back();
        strncpy(b.id, id, sizeof(b.id) - 1);
        strncpy(b.ext, imageExtensionForContentType(ct), sizeof(b.ext) - 1);
      }
    }
    return;
  }
  if (strcmp(ln, "section") == 0) {
    if (start) {
      s.sectionNesting++;
      if (s.depth == s.bodyDepth + 1) s.chapterCursor++;
    } else {
      s.sectionNesting--;
    }
    if (start) {
      const char* id = findAttr(attrs, "id");
      s.recordId(id);
    }
    return;
  }
  if (start) {
    const char* id = findAttr(attrs, "id");
    s.recordId(id);
  }
}

inline void XMLCALL structStart(void* userData, const XML_Char* name, const char** attrs) {
  StructurePass& s = *static_cast<StructurePass*>(userData);
  s.depth++;
  if (!s.sawRoot) {
    s.sawRoot = true;
    snprintf(s.rootName, sizeof(s.rootName), "%s", localName(name));
  }
  structureElement(s, localName(name), attrs, true);
}

inline void XMLCALL structEnd(void* userData, const XML_Char* name) {
  StructurePass& s = *static_cast<StructurePass*>(userData);
  structureElement(s, localName(name), nullptr, false);
  s.depth--;
}

}  // namespace fb2passes
