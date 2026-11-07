#pragma once

#include <Print.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace footnotes {

// Print-sink accumulating an item's XHTML into a string (for footnote popups:
// the source text is read straight from the book instead of the laid-out page
// cache, so words stay whole regardless of the book's hyphenation).
class StringSink final : public Print {
 public:
  std::string out;

  size_t write(uint8_t c) override {
    out += static_cast<char>(c);
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    out.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }
};

// Extracts the visible text of a footnote from its XHTML source:
// - anchors: starts at the element carrying id="<anchor>" (or the whole body
//   when empty), ends at the next peer id= attribute or </body>;
// - tags are stripped, entities decoded, whitespace collapsed.
// Our FB2 converter emits one note per section (id="noteId" on the leading
// div); EPUB notes pages with per-note ids work the same way.
inline std::string extractNoteText(const std::string& xhtml, const std::string& anchor) {
  size_t start = 0;
  if (!anchor.empty()) {
    // Find id="<anchor>" as a standalone attribute value (quotes required).
    const std::string needle = "\"" + anchor + "\"";
    size_t pos = xhtml.find(needle);
    while (pos != std::string::npos) {
      // Confirm it is an id attribute: walk back over spaces to "id=".
      size_t q = pos;
      while (q > 0 && (xhtml[q - 1] == ' ' || xhtml[q - 1] == '\t' || xhtml[q - 1] == '\n')) q--;
      if (q >= 3 && xhtml.compare(q - 3, 3, "id=") == 0) break;
      pos = xhtml.find(needle, pos + 1);
    }
    if (pos == std::string::npos) return "";
    // Roll back to the tag's opening '<' so the id attribute itself is inside
    // the stripped region, not part of the visible text.
    size_t tagOpen = xhtml.rfind('<', pos);
    if (tagOpen == std::string::npos) return "";  // malformed: no '<' before the attribute
    start = tagOpen;
  }

  size_t end = xhtml.size();
  // End at the next peer anchor or the closing body tag, whichever first. The
  // anchor's own id sits inside its opening tag, so scan for the next one only
  // after that tag closes.
  const size_t tagClose = xhtml.find('>', start);
  if (tagClose == std::string::npos) return "";
  const size_t nextId = xhtml.find("id=\"", tagClose + 1);
  const size_t bodyEnd = xhtml.find("</body>", start);
  if (nextId != std::string::npos) end = std::min(end, nextId);
  if (bodyEnd != std::string::npos) end = std::min(end, bodyEnd);

  // Strip tags; keep tag boundaries as spaces so words never glue together.
  std::string text;
  text.reserve(end - start);
  bool inTag = false;
  for (size_t i = start; i < end; i++) {
    const char c = xhtml[i];
    if (c == '<') {
      inTag = true;
      text += ' ';
      continue;
    }
    if (c == '>') {
      inTag = false;
      continue;
    }
    if (!inTag) text += c;
  }

  // Decode the entities the converter and EPUB sources actually emit.
  const auto replaceAll = [&text](const std::string& from, const std::string& to) {
    size_t p = text.find(from);
    while (p != std::string::npos) {
      text.replace(p, from.size(), to);
      p = text.find(from, p + to.size());
    }
  };
  replaceAll("&amp;", "&");
  replaceAll("&lt;", "<");
  replaceAll("&gt;", ">");
  replaceAll("&quot;", "\"");
  replaceAll("&#160;", " ");
  replaceAll("&nbsp;", " ");

  // Collapse runs of whitespace to single spaces; trim.
  std::string collapsed;
  collapsed.reserve(text.size());
  bool pendingSpace = true;  // eats leading spaces
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      pendingSpace = true;
    } else {
      if (pendingSpace && !collapsed.empty()) collapsed += ' ';
      collapsed += c;
      pendingSpace = false;
    }
  }
  return collapsed;
}

}  // namespace footnotes
