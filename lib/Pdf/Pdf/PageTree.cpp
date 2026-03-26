#include "PageTree.h"

#include <Logging.h>

#include "PdfObject.h"

namespace {

void trimInPlaceFs(PdfFixedString<PDF_DICT_VALUE_MAX>& s) {
  while (s.size() > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r' || s[0] == '\n')) {
    s.erase_prefix(1);
  }
  while (s.size() > 0) {
    const char c = s[s.size() - 1];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    s.resize(s.size() - 1);
  }
}

bool typeIs(std::string_view body, const char* name) {
  PdfFixedString<PDF_DICT_VALUE_MAX> t;
  if (!PdfObject::getDictValue("/Type", body, t)) {
    return false;
  }
  trimInPlaceFs(t);
  return t.view() == name;
}

void parseKidsRefs(std::string_view arr, PdfFixedVector<uint32_t, PDF_MAX_PAGES>& out) {
  out.clear();
  const char* p = arr.data();
  const char* end = arr.data() + arr.size();
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
  if (p < end && *p == '[') ++p;
  while (p < end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= end || *p == ']' || *p == '\0') break;
    char* e = nullptr;
    const unsigned long id = std::strtoul(p, &e, 10);
    if (e == p) {
      ++p;
      continue;
    }
    p = e;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    std::strtoul(p, &e, 10);
    p = e;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p < end && (*p == 'R' || *p == 'r')) {
      ++p;
      if (!out.push_back(static_cast<uint32_t>(id))) {
        return;
      }
    }
  }
}

}  // namespace

bool PageTree::parse(FsFile& file, const XrefTable& xref, uint32_t pagesObjId) {
  pageOffsets.clear();
  pageObjectIds.clear();

  PdfFixedVector<uint32_t, PDF_MAX_PAGES> stack;
  if (!stack.push_back(pagesObjId)) {
    return false;
  }

  while (!stack.empty() && pageOffsets.size() < PDF_MAX_PAGES) {
    const uint32_t objId = stack.back();
    stack.pop_back();

    PdfFixedString<PDF_OBJECT_BODY_MAX> body;
    if (!xref.readDictForObject(file, objId, body)) continue;

    if (typeIs(body.view(), "/Pages")) {
      PdfFixedString<PDF_DICT_VALUE_MAX> kidsStr;
      if (!PdfObject::getDictValue("/Kids", body.view(), kidsStr)) {
        continue;
      }
      PdfFixedVector<uint32_t, PDF_MAX_PAGES> kids;
      parseKidsRefs(kidsStr.view(), kids);
      for (int ki = static_cast<int>(kids.size()) - 1; ki >= 0; --ki) {
        if (!stack.push_back(kids[static_cast<size_t>(ki)])) {
          LOG_ERR("PDF", "PageTree: stack overflow");
          return false;
        }
      }
    } else if (typeIs(body.view(), "/Page")) {
      if (!pageOffsets.push_back(xref.getOffset(objId))) {
        LOG_ERR("PDF", "PageTree: too many pages");
        return false;
      }
      if (!pageObjectIds.push_back(objId)) {
        return false;
      }
    }
  }

  if (pageOffsets.empty()) {
    LOG_ERR("PDF", "PageTree: no pages");
    return false;
  }
  return true;
}

uint32_t PageTree::pageCount() const { return static_cast<uint32_t>(pageOffsets.size()); }

uint32_t PageTree::getPageOffset(uint32_t pageIndex) const {
  if (pageIndex >= pageOffsets.size()) return 0;
  return pageOffsets[pageIndex];
}

uint32_t PageTree::getPageObjectId(uint32_t pageIndex) const {
  if (pageIndex >= pageObjectIds.size()) return 0;
  return pageObjectIds[pageIndex];
}

uint32_t PageTree::pageIndexForObjectId(uint32_t objId) const {
  for (size_t i = 0; i < pageObjectIds.size(); ++i) {
    if (pageObjectIds[i] == objId) return static_cast<uint32_t>(i);
  }
  return 0;
}
