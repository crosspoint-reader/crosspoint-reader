#include "PageTree.h"

#include "PdfObject.h"
#include "PdfLog.h"

#include <cstdlib>
#include <cstring>

bool debugPageTree() {
  static int enabled = -1;
  if (enabled < 0) {
    enabled = (std::getenv("PDF_DEBUG_PAGETREE") != nullptr) ? 1 : 0;
  }
  return enabled == 1;
}

void logPageObject(uint32_t objId, const std::string_view body) {
  if (body.empty()) return;
  constexpr size_t maxLen = 180;
  const size_t n = std::min(maxLen, body.size());
  char preview[256];
  std::memcpy(preview, body.data(), n);
  preview[n] = '\0';
  if (body.size() > maxLen) {
    preview[n - 3] = '.';
    preview[n - 2] = '.';
    preview[n - 1] = '.';
  }
  LOG_ERR("PageTree", "obj=%u body=%s", objId, preview);
}

namespace {

constexpr size_t kTraversalCap = 64;

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

void parseKidsRefs(std::string_view arr, PdfFixedVector<uint32_t, kTraversalCap>& out) {
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
  pageIndexMapReady_ = false;
  for (uint32_t i = 0; i < PDF_MAX_OBJECTS; ++i) {
    pageIndexByObjectId_[i] = kInvalidPageIndex;
  }

  PdfFixedVector<uint32_t, kTraversalCap> stack;
  if (!stack.push_back(pagesObjId)) {
    return false;
  }

  while (!stack.empty() && pageOffsets.size() < PDF_MAX_PAGES) {
    const uint32_t objId = stack.back();
    stack.pop_back();

    PdfFixedString<PDF_OBJECT_BODY_MAX> body;
    if (!xref.readDictForObject(file, objId, body)) {
      if (debugPageTree()) {
        LOG_ERR("PageTree", "readDictForObject failed obj=%u", objId);
      }
      continue;
    }
    if (debugPageTree()) {
      logPageObject(objId, body.view());
    }

    if (typeIs(body.view(), "/Pages")) {
      if (debugPageTree()) {
        LOG_ERR("PageTree", "type=Pages obj=%u", objId);
      }
      PdfFixedString<PDF_DICT_VALUE_MAX> kidsStr;
      if (!PdfObject::getDictValue("/Kids", body.view(), kidsStr)) {
        if (debugPageTree()) {
          LOG_ERR("PageTree", "missing /Kids obj=%u", objId);
        }
        continue;
      }
      PdfFixedVector<uint32_t, kTraversalCap> kids;
      parseKidsRefs(kidsStr.view(), kids);
      for (int ki = static_cast<int>(kids.size()) - 1; ki >= 0; --ki) {
        if (!stack.push_back(kids[static_cast<size_t>(ki)])) {
          pdfLogErr("PageTree: stack overflow");
          return false;
        }
      }
    } else if (typeIs(body.view(), "/Page")) {
      if (debugPageTree()) {
        LOG_ERR("PageTree", "type=Page obj=%u", objId);
      }
      if (!pageOffsets.push_back(xref.getOffset(objId))) {
        pdfLogErr("PageTree: too many pages");
        return false;
      }
      const uint32_t pageIndex = static_cast<uint32_t>(pageOffsets.size() - 1);
      if (!pageObjectIds.push_back(objId)) {
        return false;
      }
      if (objId < PDF_MAX_OBJECTS) {
        pageIndexByObjectId_[objId] = static_cast<uint16_t>(pageIndex);
      }
    } else if (debugPageTree()) {
      LOG_ERR("PageTree", "unrecognized type obj=%u", objId);
    }
  }

  if (pageOffsets.empty()) {
    pdfLogErr("PageTree: no pages");
    return false;
  }
  pageIndexMapReady_ = true;
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
  if (pageIndexMapReady_ && objId < PDF_MAX_OBJECTS) {
    const uint16_t idx = pageIndexByObjectId_[objId];
    return (idx == kInvalidPageIndex) ? 0 : static_cast<uint32_t>(idx);
  }
  for (size_t i = 0; i < pageObjectIds.size(); ++i) {
    if (pageObjectIds[i] == objId) return static_cast<uint32_t>(i);
  }
  return 0;
}
