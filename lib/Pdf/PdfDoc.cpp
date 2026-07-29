#include "PdfDoc.h"

#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "PdfLexer.h"

using Kind = PdfObj::Kind;

// ---------------------------------------------------------------------------
// ByteBuf

bool ByteBuf::reserve(size_t want) {
  if (want <= cap) return true;
  size_t newCap = cap ? cap : 4096;
  while (newCap < want) newCap += newCap / 2 + 4096;
  auto nd = makeUniqueNoThrow<uint8_t[]>(newCap);
  if (!nd) {
    LOG_ERR("PDF", "OOM: %u bytes", (unsigned)newCap);
    return false;
  }
  if (len) memcpy(nd.get(), d.get(), len);
  d = std::move(nd);
  cap = newCap;
  return true;
}

bool ByteBuf::setSize(size_t newLen) {
  if (!reserve(newLen)) return false;
  len = newLen;
  return true;
}

bool ByteBuf::append(const uint8_t* src, size_t k) {
  if (!reserve(len + k)) return false;
  memcpy(d.get() + len, src, k);
  len += k;
  return true;
}

// ---------------------------------------------------------------------------
// Helpers

namespace {

uint64_t beRead(const uint8_t* p, uint32_t width) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < width; i++) v = (v << 8) | p[i];
  return v;
}

uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
  const int pr = (int)a + (int)b - (int)c;
  const int pa = pr > a ? pr - a : a - pr;
  const int pb = pr > b ? pr - b : b - pr;
  const int pc = pr > c ? pr - c : c - pr;
  if (pa <= pb && pa <= pc) return a;
  return pb <= pc ? b : c;
}

int hexVal(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// File access

size_t PdfDoc::readUpTo(uint32_t ofs, void* buf, size_t len) {
  if (ofs >= fsize) return 0;
  if ((uint64_t)ofs + len > fsize) len = fsize - ofs;
  if (!file.seek(ofs)) return 0;
  const int got = file.read(buf, len);
  return got < 0 ? 0 : (size_t)got;
}

// ---------------------------------------------------------------------------
// Open

bool PdfDoc::open(const std::string& path, std::string* userErrorOut) {
  auto fail = [&](const char* msg) {
    if (userErrorOut) *userErrorOut = msg;
    LOG_ERR("PDF", "%s", msg);
    return false;
  };

  if (!Storage.openFileForRead("PDF", path, file)) return fail("cannot open PDF file");
  const size_t fs = file.fileSize();
  if (fs < 64) return fail("not a PDF");
  if (fs > 64u * 1024 * 1024) return fail("PDF too large (max 64MB)");
  fsize = (uint32_t)fs;

  {  // %PDF- header; tolerate junk in the first 1KB
    if (!window.setSize(1024)) return fail("out of memory");
    const size_t got = readUpTo(0, window.data(), 1024);
    const uint8_t* head = window.data();
    bool found = false;
    for (size_t i = 0; i + 5 <= got; i++) {
      if (memcmp(head + i, "%PDF-", 5) == 0) {
        headerOfs = (uint32_t)i;
        found = true;
        break;
      }
    }
    if (!found) return fail("not a PDF");
  }

  uint32_t sx = 0;
  if (!findStartXref(sx)) return fail("damaged PDF (no startxref)");
  if (!parseXrefChain(sx)) return fail("damaged PDF (bad xref)");
  if (encrypted) return fail("PDF is encrypted");
  if (rootNum == 0) return fail("damaged PDF (no catalog)");

  PdfObj catalog;
  if (!loadObject(rootNum, catalog) || !catalog.isDict()) return fail("damaged PDF (no catalog)");
  const PdfObj* pagesRef = catalog.find("Pages");
  if (!pagesRef || pagesRef->kind != Kind::Ref) return fail("damaged PDF (no page tree)");

  pageList.reserve(64);
  PdfObj noRes;
  collectPages(pagesRef->ref, noRes, 0);
  if (pageList.empty()) return fail("PDF has no pages");

  if (infoNum) {
    PdfObj inf;
    if (loadObject(infoNum, inf) && inf.isDict()) infoDict = std::move(inf);
  }
  LOG_INF("PDF", "opened: %u bytes, %u pages, %u xref slots", (unsigned)fsize, (unsigned)pageList.size(),
          (unsigned)xref.size());
  return true;
}

bool PdfDoc::findStartXref(uint32_t& ofs) {
  constexpr uint32_t TAIL = 2048;
  if (!window.setSize(TAIL)) return false;
  uint8_t* tail = window.data();
  const uint32_t start = fsize > TAIL ? fsize - TAIL : 0;
  const size_t got = readUpTo(start, tail, TAIL);
  if (got < 20) return false;
  int found = -1;
  for (int i = (int)got - 9; i >= 0; i--) {
    if (memcmp(tail + i, "startxref", 9) == 0) {
      found = i;
      break;
    }
  }
  if (found < 0) return false;
  size_t p = (size_t)found + 9;
  while (p < got && PdfLexer::isWs(tail[p])) p++;
  uint64_t v = 0;
  bool digits = false;
  while (p < got && tail[p] >= '0' && tail[p] <= '9') {
    v = v * 10 + (uint64_t)(tail[p] - '0');
    digits = true;
    p++;
    if (v > 0xFFFFFFFFull) return false;
  }
  if (!digits || v >= fsize) return false;
  ofs = (uint32_t)v;
  return true;
}

// ---------------------------------------------------------------------------
// Xref

bool PdfDoc::parseXrefChain(uint32_t startOfs) {
  uint32_t visited[MAX_XREF_SECTIONS];
  int nv = 0;
  bool any = false;
  uint32_t ofs = startOfs;
  while (ofs && nv < MAX_XREF_SECTIONS) {
    bool seen = false;
    for (int i = 0; i < nv; i++) {
      if (visited[i] == ofs) seen = true;
    }
    if (seen) break;  // cycle in /Prev chain
    visited[nv++] = ofs;
    uint32_t prev = 0;
    if (!parseXrefAt(ofs, prev)) break;
    any = true;
    ofs = prev;
  }
  return any;
}

bool PdfDoc::parseXrefAt(uint32_t ofs, uint32_t& prevOut) {
  for (int attempt = 0; attempt < 2; attempt++) {
    const uint32_t at = attempt == 0 ? ofs : ofs + headerOfs;  // retry offset-base shift for junk-prefixed files
    if (at >= fsize) return false;
    uint8_t peekBuf[64];
    const size_t got = readUpTo(at, peekBuf, sizeof(peekBuf));
    if (got < 4) return false;
    PdfLexer lx(peekBuf, got);
    lx.skipWs();
    uint32_t xstm = 0;
    bool ok;
    if (lx.keyword("xref")) {
      ok = parseClassicXref(at, prevOut, xstm);
    } else {
      ok = parseXrefStream(at, prevOut);
    }
    if (ok) {
      if (xstm) {  // hybrid-reference file: the classic section points at an xref stream
        uint32_t dummy = 0;
        if (!parseXrefStream(xstm, dummy) && headerOfs) parseXrefStream(xstm + headerOfs, dummy);
      }
      return true;
    }
    if (headerOfs == 0) return false;
  }
  return false;
}

bool PdfDoc::parseClassicXref(uint32_t ofs, uint32_t& prevOut, uint32_t& xrefStmOut) {
  prevOut = 0;
  xrefStmOut = 0;
  uint8_t buf[160];  // subsection headers + "trailer" keyword only
  size_t got = readUpTo(ofs, buf, sizeof(buf));
  if (got < 4) return false;
  {
    PdfLexer lx(buf, got);
    lx.skipWs();
    if (!lx.keyword("xref")) return false;
    ofs += (uint32_t)lx.pos();
  }
  for (int sub = 0; sub < 8192; sub++) {
    got = readUpTo(ofs, buf, sizeof(buf));
    if (got == 0) return false;
    PdfLexer h(buf, got);
    h.skipWs();
    if (h.keyword("trailer")) {
      PdfObj tr;
      if (!parseDictAt(ofs + (uint32_t)h.pos(), tr)) return false;
      parseTrailerDict(tr, prevOut, xrefStmOut);
      return true;
    }
    uint32_t start = 0, count = 0;
    if (!h.readUInt(start) || !h.readUInt(count)) return false;
    if (start > MAX_OBJECTS || count > MAX_OBJECTS) return false;
    h.skipWs();  // cursor now at the first entry byte
    ofs += (uint32_t)h.pos();
    if (count && !parseXrefEntries(ofs, start, count)) return false;
  }
  return false;
}

// Streams `count` xref records from absolute file position *absInOut. Records
// are nominally 20 bytes ("nnnnnnnnnn ggggg n\r\n") but token-parsing them
// tolerates the 19-byte single-EOL variants sloppy writers emit.
bool PdfDoc::parseXrefEntries(uint32_t& absInOut, uint32_t start, uint32_t count) {
  uint32_t abs = absInOut;
  uint32_t idx = start;
  uint32_t remaining = count;
  constexpr size_t CHUNK = 2048;
  if (!window.setSize(CHUNK)) return false;
  uint8_t* chunk = window.data();
  while (remaining) {
    const size_t got = readUpTo(abs, chunk, CHUNK);
    if (got < 18) return false;
    PdfLexer lx(chunk, got);
    bool progress = false;
    while (remaining) {
      if (got - lx.pos() < 20 && abs + got < fsize) break;  // keep one full record of headroom, then refill
      uint32_t off = 0, gen = 0;
      if (!lx.readUInt(off) || !lx.readUInt(gen)) return false;
      lx.skipWs();
      if (lx.atEnd()) return false;
      const uint8_t t = chunk[lx.pos()];
      if (t != 'n' && t != 'f') return false;
      lx.setPos(lx.pos() + 1);
      storeXref(idx, t == 'n' ? 1 : 0, off, (uint16_t)(gen > 65535 ? 65535 : gen));
      idx++;
      remaining--;
      progress = true;
    }
    if (!progress) return false;
    abs += (uint32_t)lx.pos();
  }
  absInOut = abs;
  return true;
}

bool PdfDoc::parseXrefStream(uint32_t ofs, uint32_t& prevOut) {
  prevOut = 0;
  PdfObj stm;
  if (!loadObjectAt(ofs, 0, stm, 0) || stm.kind != Kind::Stream) return false;
  const PdfObj* w = stm.find("W");
  if (!w || w->kind != Kind::Array || w->arr.size() < 3) return false;
  uint32_t ws[3];
  for (int i = 0; i < 3; i++) {
    if (!w->arr[(size_t)i].isNum()) return false;
    const int32_t v = w->arr[(size_t)i].asInt();
    if (v < 0 || v > 8) return false;
    ws[i] = (uint32_t)v;
  }
  if (ws[1] == 0) return false;

  uint32_t xstmDummy = 0;
  parseTrailerDict(stm, prevOut, xstmDummy);
  uint32_t size = 0;
  if (const PdfObj* s = stm.find("Size"); s && s->isNum()) size = (uint32_t)s->num;

  ByteBuf data;
  if (!getStreamData(stm, data, MAX_DECODED)) return false;

  // /Index pairs, default [0 Size]
  uint32_t pairs[64][2];
  size_t nPairs = 0;
  const PdfObj* idx = stm.find("Index");
  if (idx && idx->kind == Kind::Array) {
    for (size_t i = 0; i + 1 < idx->arr.size() && nPairs < 64; i += 2) {
      if (!idx->arr[i].isNum() || !idx->arr[i + 1].isNum()) break;
      pairs[nPairs][0] = (uint32_t)idx->arr[i].num;
      pairs[nPairs][1] = (uint32_t)idx->arr[i + 1].num;
      nPairs++;
    }
  } else {
    pairs[0][0] = 0;
    pairs[0][1] = size;
    nPairs = 1;
  }

  const size_t entryW = ws[0] + ws[1] + ws[2];
  size_t pos = 0;
  for (size_t pi = 0; pi < nPairs; pi++) {
    const uint32_t first = pairs[pi][0];
    const uint32_t cnt = pairs[pi][1] > MAX_OBJECTS ? MAX_OBJECTS : pairs[pi][1];
    for (uint32_t i = 0; i < cnt; i++) {
      if (pos + entryW > data.size()) return true;  // truncated stream: keep what we have
      const uint8_t* e = data.data() + pos;
      pos += entryW;
      const uint64_t f1 = ws[0] ? beRead(e, ws[0]) : 1;  // type defaults to 1 when W[0]==0
      const uint64_t f2 = beRead(e + ws[0], ws[1]);
      const uint64_t f3 = ws[2] ? beRead(e + ws[0] + ws[1], ws[2]) : 0;
      const uint16_t aux = (uint16_t)(f3 > 65535 ? 65535 : f3);
      if (f1 == 0) {
        storeXref(first + i, 0, 0, 0);
      } else if (f1 == 1) {
        storeXref(first + i, 1, (uint32_t)f2, aux);
      } else if (f1 == 2) {
        storeXref(first + i, 2, (uint32_t)f2, aux);
      }
    }
  }
  return true;
}

void PdfDoc::parseTrailerDict(const PdfObj& tr, uint32_t& prevOut, uint32_t& xrefStmOut) {
  if (const PdfObj* r = tr.find("Root"); r && r->kind == Kind::Ref && rootNum == 0) rootNum = r->ref;
  if (const PdfObj* i = tr.find("Info"); i && i->kind == Kind::Ref && infoNum == 0) infoNum = i->ref;
  if (const PdfObj* e = tr.find("Encrypt"); e && e->kind != Kind::Null) encrypted = true;
  if (const PdfObj* p = tr.find("Prev"); p && p->isNum()) prevOut = (uint32_t)p->num;
  if (const PdfObj* x = tr.find("XRefStm"); x && x->isNum()) xrefStmOut = (uint32_t)x->num;
}

bool PdfDoc::parseDictAt(uint32_t ofs, PdfObj& out) {
  for (size_t wsize = 4096; wsize <= 256u * 1024; wsize *= 4) {
    size_t want = wsize;
    const bool lastPossible = (uint64_t)ofs + want >= fsize;
    if (lastPossible) want = fsize - ofs;
    if (want == 0) return false;
    if (!window.setSize(want)) return false;
    if (readUpTo(ofs, window.data(), want) != want) return false;
    PdfLexer lx(window.data(), want);
    out.reset();
    if (lx.parseObject(out)) return out.isDict();
    if (!lx.ranOffEnd() || lastPossible) return false;
  }
  return false;
}

void PdfDoc::storeXref(uint32_t num, uint8_t type, uint32_t val, uint16_t aux) {
  if (num >= MAX_OBJECTS) return;
  if (num >= xref.size()) {
    size_t newSize = xref.size() ? xref.size() : 256;
    while (newSize <= num) newSize *= 2;
    if (newSize > MAX_OBJECTS) newSize = MAX_OBJECTS;
    xref.resize(newSize);
  }
  XrefEntry& e = xref[num];
  // Newest-first chain walk: first definition wins. Exception: a free entry
  // may be superseded by a real one so hybrid-reference /XRefStm sections can
  // fill in the objects their classic table lists as free.
  if (e.type == 0xFF || (e.type == 0 && type != 0)) {
    e.type = type;
    e.val = val;
    e.aux = aux;
  }
}

// ---------------------------------------------------------------------------
// Objects

bool PdfDoc::loadObject(uint32_t num, PdfObj& out, int depth) {
  if (depth > 8 || num == 0 || num >= xref.size()) return false;
  const XrefEntry e = xref[num];
  if (e.type == 1) {
    if (loadObjectAt(e.val, num, out, depth)) return true;
    if (headerOfs && loadObjectAt(e.val + headerOfs, num, out, depth)) return true;
    return false;
  }
  if (e.type == 2) return loadFromObjStm(e.val, num, out, depth);
  return false;
}

const PdfObj* PdfDoc::resolve(const PdfObj* o, PdfObj& storage, int depth) {
  if (!o) return nullptr;
  for (int guard = 0; o->kind == Kind::Ref && guard < 8; guard++) {
    const uint32_t num = o->ref;
    if (!loadObject(num, storage, depth)) return nullptr;
    o = &storage;
  }
  return o->kind == Kind::Ref ? nullptr : o;
}

bool PdfDoc::loadObjectAt(uint32_t ofs, uint32_t expectNum, PdfObj& out, int depth) {
  if (ofs >= fsize) return false;
  for (size_t wsize = 8192; wsize <= MAX_OBJ_WINDOW; wsize *= 8) {
    size_t want = wsize;
    const bool lastPossible = (uint64_t)ofs + want >= fsize;
    if (lastPossible) want = fsize - ofs;
    if (want == 0) return false;
    if (!window.setSize(want)) return false;
    if (readUpTo(ofs, window.data(), want) != want) return false;

    PdfLexer lx(window.data(), want);
    uint32_t num = 0, gen = 0;
    if (!lx.readUInt(num) || !lx.readUInt(gen)) return false;
    lx.skipWs();
    if (!lx.keyword("obj")) return false;
    if (expectNum && num != expectNum) return false;

    out.reset();
    if (!lx.parseObject(out)) {
      if (lx.ranOffEnd() && !lastPossible) continue;  // window too small
      return false;
    }
    if (out.kind != Kind::Dict) return true;

    // Streams: dict followed by `stream` + EOL. The keyword itself may
    // straddle the window end — regrow rather than misparse.
    if (want - lx.pos() < 16 && !lastPossible) continue;
    const size_t save = lx.pos();
    lx.skipWs();
    if (!lx.keyword("stream")) {
      lx.setPos(save);
      return true;
    }
    size_t sp = lx.pos();
    if (sp < want && window.data()[sp] == '\r') sp++;
    if (sp < want && window.data()[sp] == '\n') sp++;
    out.kind = Kind::Stream;
    out.streamOfs = ofs + (uint32_t)sp;

    uint32_t slen = 0;
    if (const PdfObj* L = out.find("Length")) {
      if (L->isNum()) {
        slen = L->num > 0 ? (uint32_t)L->num : 0;
      } else if (L->kind == Kind::Ref && depth < 6) {
        PdfObj lo;
        if (loadObject(L->ref, lo, depth + 1) && lo.isNum() && lo.num > 0) slen = (uint32_t)lo.num;
      }
    }
    if (out.streamOfs > fsize) out.streamOfs = fsize;
    if ((uint64_t)out.streamOfs + slen > fsize) slen = fsize - out.streamOfs;
    out.streamLen = slen;
    return true;
  }
  return false;
}

bool PdfDoc::loadFromObjStm(uint32_t stmNum, uint32_t wantNum, PdfObj& out, int depth) {
  if (depth > 8) return false;
  ObjStmSlot* slot = nullptr;
  for (auto& s : objStmCache) {
    if (s.stmNum == stmNum && !s.data.empty()) slot = &s;
  }
  if (!slot) {
    PdfObj stm;
    if (!loadObject(stmNum, stm, depth + 1) || stm.kind != Kind::Stream) return false;
    PdfObj tmpN, tmpF;
    const PdfObj* rn = resolve(stm.find("N"), tmpN, depth + 1);
    const PdfObj* rf = resolve(stm.find("First"), tmpF, depth + 1);
    if (!rn || !rf || !rn->isNum() || !rf->isNum()) return false;

    slot = &objStmCache[0];
    for (auto& s : objStmCache) {
      if (s.age < slot->age) slot = &s;  // oldest (empty slots have age 0)
    }
    slot->stmNum = 0;
    slot->data.clear();
    if (!getStreamData(stm, slot->data, MAX_DECODED)) return false;
    slot->stmNum = stmNum;
    slot->first = rf->num > 0 ? (uint32_t)rf->num : 0;
    slot->count = rn->num > 0 ? (uint32_t)rn->num : 0;
  }
  slot->age = ++objStmClock;

  if (slot->first > slot->data.size()) return false;
  PdfLexer hx(slot->data.data(), slot->first);
  uint32_t foundOfs = 0;
  bool found = false;
  const uint32_t cnt = slot->count > 16384 ? 16384 : slot->count;
  for (uint32_t i = 0; i < cnt; i++) {
    uint32_t on = 0, oo = 0;
    if (!hx.readUInt(on) || !hx.readUInt(oo)) break;
    if (on == wantNum) {
      foundOfs = oo;
      found = true;
      break;
    }
  }
  if (!found) return false;
  const uint64_t objPos = (uint64_t)slot->first + foundOfs;
  if (objPos >= slot->data.size()) return false;
  PdfLexer lx(slot->data.data(), slot->data.size());
  lx.setPos((size_t)objPos);
  out.reset();
  return lx.parseObject(out);
}

// ---------------------------------------------------------------------------
// Stream decoding

bool PdfDoc::getStreamData(const PdfObj& stream, ByteBuf& out, size_t maxOut) {
  out.clear();
  if (stream.kind != Kind::Stream || stream.streamLen == 0) return false;

  PdfObj filtStore, parmStore;
  const PdfObj* filt = resolve(stream.find("Filter"), filtStore);
  const PdfObj* parmsRoot = stream.find("DecodeParms");
  if (!parmsRoot) parmsRoot = stream.find("DP");
  const PdfObj* parms = resolve(parmsRoot, parmStore);

  ByteBuf raw;
  if (!raw.setSize(stream.streamLen)) return false;
  if (readUpTo(stream.streamOfs, raw.data(), stream.streamLen) != stream.streamLen) return false;

  if (!filt || filt->kind == Kind::Null) {
    if (raw.size() > maxOut) return false;
    out = std::move(raw);
    return true;
  }

  const PdfObj* filters[4];
  const PdfObj* parmList[4];
  int nf = 0;
  if (filt->kind == Kind::Name) {
    filters[0] = filt;
    parmList[0] = parms && parms->isDict() ? parms : nullptr;
    nf = 1;
  } else if (filt->kind == Kind::Array) {
    if (filt->arr.size() > 4) return false;
    for (size_t i = 0; i < filt->arr.size(); i++) {
      filters[nf] = &filt->arr[i];
      const PdfObj* pm = nullptr;
      if (parms) {
        if (parms->kind == Kind::Array && i < parms->arr.size())
          pm = &parms->arr[i];
        else if (parms->isDict() && i == 0)
          pm = parms;
      }
      parmList[nf++] = pm;
    }
  } else {
    return false;
  }

  ByteBuf cur = std::move(raw);
  for (int i = 0; i < nf; i++) {
    PdfObj fStore, pStore;
    const PdfObj* f = resolve(filters[i], fStore);
    const PdfObj* pm = resolve(parmList[i], pStore);
    if (!f || f->kind != Kind::Name) return false;
    ByteBuf next;
    if (!applyFilter(f->str, pm && pm->isDict() ? pm : nullptr, cur, next, maxOut)) return false;
    cur = std::move(next);
  }
  out = std::move(cur);
  return true;
}

bool PdfDoc::applyFilter(const std::string& name, const PdfObj* parms, ByteBuf& in, ByteBuf& out, size_t maxOut) {
  if (name == "FlateDecode" || name == "Fl") {
    if (!inflateAll(in.data(), in.size(), out, maxOut)) return false;
    return applyPredictor(parms, out);
  }
  if (name == "ASCIIHexDecode" || name == "AHx") {
    if (!out.reserve(in.size() / 2 + 2)) return false;
    int hi = -1;
    for (size_t i = 0; i < in.size(); i++) {
      const uint8_t c = in.data()[i];
      if (c == '>') break;
      const int v = hexVal(c);
      if (v < 0) continue;
      if (hi < 0) {
        hi = v;
      } else {
        const uint8_t byte = (uint8_t)((hi << 4) | v);
        if (!out.append(&byte, 1)) return false;
        hi = -1;
      }
    }
    if (hi >= 0) {
      const uint8_t byte = (uint8_t)(hi << 4);
      if (!out.append(&byte, 1)) return false;
    }
    return true;
  }
  if (name == "RunLengthDecode" || name == "RL") {
    size_t i = 0;
    while (i < in.size() && out.size() <= maxOut) {
      const uint8_t l = in.data()[i++];
      if (l == 128) break;  // EOD
      if (l < 128) {
        const size_t k = (size_t)l + 1;
        if (i + k > in.size()) break;
        if (!out.append(in.data() + i, k)) return false;
        i += k;
      } else {
        if (i >= in.size()) break;
        const uint8_t rep = in.data()[i++];
        const size_t k = 257 - (size_t)l;
        if (!out.reserve(out.size() + k)) return false;
        memset(out.data() + out.size(), rep, k);
        out.advance(k);
      }
    }
    return out.size() <= maxOut;
  }
  // DCTDecode/JPXDecode are images (ignored — text-reflow converter), LZW and
  // the rest are unsupported: the stream degrades to "no data".
  LOG_DBG("PDF", "unsupported filter: %s", name.c_str());
  return false;
}

bool PdfDoc::inflateAll(const uint8_t* src, size_t len, ByteBuf& out, size_t maxOut) {
  // PDF FlateDecode is zlib-wrapped; a raw-deflate retry rescues the sloppy
  // generators that omit the header.
  for (int wrapped = 1; wrapped >= 0; wrapped--) {
    InflateStream inf;
    if (!inf.init(true)) {
      LOG_ERR("PDF", "OOM: inflate state");
      return false;
    }
    if (wrapped) inf.setZlibWrapped();
    inf.setSource(src, len);
    out.clear();
    if (!out.reserve(len * 2 + 8192)) return false;
    const size_t maxIter = maxOut / 4096 + 16;
    bool errored = false;
    for (size_t it = 0; it < maxIter; it++) {
      if (!out.reserve(out.size() + 16384)) return false;
      size_t produced = 0;
      const auto st = inf.readAtMost(out.data() + out.size(), 16384, &produced);
      out.advance(produced);
      if (st == InflateStream::Status::Done) return !out.empty();
      if (st == InflateStream::Status::Error) {
        errored = true;
        break;
      }
      if (out.size() > maxOut) {
        LOG_ERR("PDF", "stream exceeds %u bytes decoded", (unsigned)maxOut);
        return false;
      }
    }
    if (!errored) return false;      // hit iteration cap: oversized/corrupt
    if (!out.empty()) return false;  // mid-stream corruption: don't retry raw
  }
  return false;
}

bool PdfDoc::applyPredictor(const PdfObj* parms, ByteBuf& buf) {
  if (!parms) return true;
  auto intOf = [&](const char* k, int def) {
    PdfObj st;
    const PdfObj* v = resolve(parms->find(k), st);
    return v && v->isNum() ? (int)v->num : def;
  };
  const int pred = intOf("Predictor", 1);
  if (pred <= 1) return true;
  int colors = intOf("Colors", 1);
  int bpc = intOf("BitsPerComponent", 8);
  int columns = intOf("Columns", 1);
  if (colors < 1 || colors > 32) colors = 1;
  if (bpc < 1 || bpc > 16) bpc = 8;
  if (columns < 1 || columns > (1 << 20)) return false;
  size_t bpp = ((size_t)colors * bpc + 7) / 8;
  if (bpp == 0) bpp = 1;
  const size_t rowBytes = ((size_t)columns * colors * bpc + 7) / 8;
  if (rowBytes == 0 || rowBytes > (1u << 20)) return false;

  uint8_t* d = buf.data();
  const size_t n = buf.size();

  if (pred == 2) {  // TIFF horizontal differencing
    if (bpc != 8) return false;
    for (size_t r = 0; r + rowBytes <= n; r += rowBytes) {
      for (size_t i = bpp; i < rowBytes; i++) d[r + i] = (uint8_t)(d[r + i] + d[r + i - bpp]);
    }
    return true;
  }
  if (pred < 10 || pred > 15) return false;

  // PNG predictors: rows of (tag + rowBytes), unfiltered + compacted in place.
  const size_t inRow = 1 + rowBytes;
  const size_t rows = n / inRow;
  if (rows == 0) return false;
  auto prevRow = makeUniqueNoThrow<uint8_t[]>(rowBytes);  // value-initialized to zero
  if (!prevRow) return false;
  size_t outLen = 0;
  for (size_t r = 0; r < rows; r++) {
    const uint8_t* src = d + r * inRow;
    const uint8_t tag = src[0];
    uint8_t* dst = d + outLen;  // dst always trails src: in-place is safe
    for (size_t i = 0; i < rowBytes; i++) {
      const uint8_t raw = src[1 + i];
      const uint8_t left = i >= bpp ? dst[i - bpp] : 0;
      const uint8_t up = prevRow[i];
      const uint8_t ul = i >= bpp ? prevRow[i - bpp] : 0;
      uint8_t v;
      switch (tag) {
        case 1:
          v = (uint8_t)(raw + left);
          break;  // Sub
        case 2:
          v = (uint8_t)(raw + up);
          break;  // Up
        case 3:
          v = (uint8_t)(raw + (uint8_t)(((int)left + up) / 2));
          break;  // Average
        case 4:
          v = (uint8_t)(raw + paeth(left, up, ul));
          break;  // Paeth
        default:
          v = raw;
          break;  // None / unknown
      }
      dst[i] = v;
    }
    memcpy(prevRow.get(), dst, rowBytes);
    outLen += rowBytes;
  }
  buf.truncate(outLen);
  return true;
}

// ---------------------------------------------------------------------------
// Page tree

bool PdfDoc::collectPages(uint32_t nodeNum, const PdfObj& inheritedRes, int depth) {
  if (depth > 64 || pageList.size() >= MAX_PAGES) return false;
  const auto it = std::lower_bound(visitedNodes.begin(), visitedNodes.end(), nodeNum);
  if (it != visitedNodes.end() && *it == nodeNum) return false;  // cycle / shared subtree
  if (visitedNodes.size() > 20000) return false;
  visitedNodes.insert(it, nodeNum);

  PdfObj node;
  if (!loadObject(nodeNum, node) || !node.isDict()) return false;
  const PdfObj* res = node.find("Resources");
  const PdfObj& effRes = res ? *res : inheritedRes;
  // Note: /MediaBox is inheritable too but unused by text reflow — not carried.

  PdfObj kidsStore;
  const PdfObj* kids = resolve(node.find("Kids"), kidsStore);
  if (kids && kids->kind == Kind::Array) {
    for (const auto& kid : kids->arr) {
      if (pageList.size() >= MAX_PAGES) break;
      if (kid.kind == Kind::Ref) collectPages(kid.ref, effRes, depth + 1);
    }
    return true;
  }
  const PdfObj* type = node.find("Type");
  if (type && type->isName("Pages")) return true;  // intermediate node without kids
  pageList.push_back({nodeNum, effRes});
  return true;
}
