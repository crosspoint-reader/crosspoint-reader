#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "PdfObject.h"

// Growable byte buffer with nothrow allocation. std::vector aborts on OOM
// under -fno-exceptions; this returns false instead, which matters for the
// multi-megabyte decoded-stream buffers this library moves around.
class ByteBuf {
 public:
  ByteBuf() = default;
  ByteBuf(ByteBuf&&) = default;
  ByteBuf& operator=(ByteBuf&&) = default;
  ByteBuf(const ByteBuf&) = delete;
  ByteBuf& operator=(const ByteBuf&) = delete;

  uint8_t* data() { return d.get(); }
  const uint8_t* data() const { return d.get(); }
  size_t size() const { return len; }
  bool empty() const { return len == 0; }
  void clear() { len = 0; }
  size_t capacity() const { return cap; }

  bool reserve(size_t want);    // grow capacity to >= want; false on OOM
  bool setSize(size_t newLen);  // reserve + set length (bytes uninitialized on grow)
  bool append(const uint8_t* src, size_t k);
  void advance(size_t k) { len += k; }  // after an external write into reserved space
  void truncate(size_t newLen) {
    if (newLen < len) len = newLen;
  }

 private:
  std::unique_ptr<uint8_t[]> d;
  size_t len = 0;
  size_t cap = 0;
};

// PDF document reader: xref chains (classic tables and 1.5+ xref streams with
// PNG/TIFF predictors), object and object-stream loading, stream filters
// (FlateDecode, ASCIIHexDecode, RunLengthDecode), and the page-tree walk.
// Read-only; no RTTI, no exceptions; every loop bounded. Public entry point
// for conversions is PdfToEpub.
class PdfDoc {
 public:
  struct Page {
    uint32_t objNum = 0;
    PdfObj resources;  // effective (inherited) /Resources: Ref, Dict, or Null
  };

  static constexpr size_t MAX_DECODED = 4u * 1024 * 1024;  // decoded stream cap

  // Opens + indexes the document. On failure fills userErrorOut with a
  // user-showable message and returns false.
  bool open(const std::string& path, std::string* userErrorOut);

  // Load object N. False = unusable object (caller degrades gracefully).
  bool loadObject(uint32_t num, PdfObj& out, int depth = 0);
  // Follow Ref indirection (loads into storage); other kinds pass through
  // unchanged. Returns nullptr when the chain is broken.
  const PdfObj* resolve(const PdfObj* o, PdfObj& storage, int depth = 0);
  // Decode a Stream object's data through its /Filter chain. False when the
  // stream is corrupt or uses an unsupported filter (DCT/JPX/LZW/...).
  bool getStreamData(const PdfObj& stream, ByteBuf& out, size_t maxOut);

  const std::vector<Page>& pages() const { return pageList; }
  const PdfObj& info() const { return infoDict; }
  uint32_t fileSize() const { return fsize; }

 private:
  static constexpr uint32_t MAX_OBJECTS = 262144;
  static constexpr int MAX_XREF_SECTIONS = 32;
  static constexpr size_t MAX_PAGES = 5000;
  static constexpr size_t MAX_OBJ_WINDOW = 2u * 1024 * 1024;

  struct XrefEntry {
    uint32_t val = 0;     // type 1: file offset; type 2: objstm object number
    uint16_t aux = 0;     // type 1: generation;  type 2: index within objstm
    uint8_t type = 0xFF;  // 0 free, 1 offset, 2 in-objstm, 0xFF unset
  };

  struct ObjStmSlot {
    uint32_t stmNum = 0;
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t age = 0;
    ByteBuf data;
  };

  size_t readUpTo(uint32_t ofs, void* buf, size_t len);  // clamped read, returns bytes read
  bool findStartXref(uint32_t& ofs);
  bool parseXrefChain(uint32_t startOfs);
  bool parseXrefAt(uint32_t ofs, uint32_t& prevOut);
  bool parseClassicXref(uint32_t ofs, uint32_t& prevOut, uint32_t& xrefStmOut);
  bool parseXrefEntries(uint32_t& absInOut, uint32_t start, uint32_t count);
  bool parseXrefStream(uint32_t ofs, uint32_t& prevOut);
  void parseTrailerDict(const PdfObj& dict, uint32_t& prevOut, uint32_t& xrefStmOut);
  bool parseDictAt(uint32_t ofs, PdfObj& out);
  void storeXref(uint32_t num, uint8_t type, uint32_t val, uint16_t aux);
  bool loadObjectAt(uint32_t ofs, uint32_t expectNum, PdfObj& out, int depth);
  bool loadFromObjStm(uint32_t stmNum, uint32_t wantNum, PdfObj& out, int depth);
  bool applyFilter(const std::string& name, const PdfObj* parms, ByteBuf& in, ByteBuf& out, size_t maxOut);
  bool applyPredictor(const PdfObj* parms, ByteBuf& buf);
  bool inflateAll(const uint8_t* src, size_t len, ByteBuf& out, size_t maxOut);
  bool collectPages(uint32_t nodeNum, const PdfObj& inheritedRes, int depth);

  HalFile file;
  uint32_t fsize = 0;
  uint32_t headerOfs = 0;  // offset of %PDF- when junk precedes the header
  std::vector<XrefEntry> xref;
  uint32_t rootNum = 0;
  uint32_t infoNum = 0;
  bool encrypted = false;
  PdfObj infoDict;
  std::vector<Page> pageList;
  std::vector<uint32_t> visitedNodes;  // page-tree cycle guard, kept sorted
  ObjStmSlot objStmCache[4];           // ponytail: tiny LRU; grow if ObjStm-heavy PDFs thrash
  uint32_t objStmClock = 0;
  ByteBuf window;  // reused object-parse window
};
