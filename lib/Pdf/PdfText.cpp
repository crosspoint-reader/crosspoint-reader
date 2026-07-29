#include "PdfText.h"

#include <Logging.h>
#include <Memory.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "PdfLexer.h"
#include "PdfUtil.h"

using Kind = PdfObj::Kind;

namespace {

constexpr int MAX_FORM_DEPTH = 8;
constexpr size_t MAX_OPERANDS = 32;
constexpr size_t MAX_GS_STACK = 32;
constexpr float TJ_SPACE_GAP = -180.0f;  // thousandths of text space

struct Mat {
  float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

Mat mul(const Mat& m, const Mat& n) {  // m x n
  Mat r;
  r.a = m.a * n.a + m.b * n.c;
  r.b = m.a * n.b + m.b * n.d;
  r.c = m.c * n.a + m.d * n.c;
  r.d = m.c * n.b + m.d * n.d;
  r.e = m.e * n.a + m.f * n.c + n.e;
  r.f = m.e * n.b + m.f * n.d + n.f;
  return r;
}

// Paragraph assembler: turns positioned show-ops into <p> blocks.
struct Assembler {
  std::string* out = nullptr;
  bool any = false;
  bool open = false;
  float lastY = 0;
  float lastSize = 12;
  float lastEndX = 0;

  void show(float x, float y, float size, const std::string& utf8, size_t glyphs) {
    if (utf8.empty()) return;
    if (size <= 0.01f) size = 12;
    if (!open) {
      *out += "<p>";
      open = true;
    } else {
      const float dy = y - lastY;
      const float ref = lastSize;
      if (dy > 1.6f * ref || dy < -1.6f * ref) {
        *out += "</p>\n<p>";
      } else if (dy < -0.4f * ref) {
        // next line: soft space, joining hyphenated words
        if (!out->empty() && out->back() == '-') {
          out->pop_back();
        } else {
          *out += ' ';
        }
      } else {
        // same line (small |dy| covers super/subscripts): gap -> space.
        // Expected advance approximated as 0.5 * size * glyphs (crude, fine).
        if (x > lastEndX + 0.3f * size) *out += ' ';
      }
    }
    pdfXmlEscapeAppend(*out, utf8.data(), utf8.size());
    any = true;
    lastY = y;
    lastSize = size;
    lastEndX = x + 0.5f * size * (float)glyphs;
  }

  void finish() {
    if (open) *out += "</p>\n";
    open = false;
  }
};

struct Interp {
  PdfDoc& doc;
  PdfFontCache& fonts;
  Assembler& asmb;
  const PdfText::ImageSink* images = nullptr;
  int lostShows = 0;
  int totalShows = 0;

  bool run(const uint8_t* data, size_t len, const PdfObj* res, const Mat& ctm0, int depth);
  void invokeXObject(const std::string& name, const PdfObj* res, const Mat& ctm, int depth);
  const PdfFont* lookupFont(const PdfObj* res, const std::string& name);
};

float numOf(const PdfObj& o) { return o.isNum() ? (float)o.num : 0.0f; }

const PdfFont* Interp::lookupFont(const PdfObj* res, const std::string& name) {
  if (!res) return nullptr;
  PdfObj mapStore;
  const PdfObj* fontMap = doc.resolve(res->find("Font"), mapStore);
  if (!fontMap || !fontMap->isDict()) return nullptr;
  return fonts.get(doc, fontMap->find(name.c_str()));
}

void Interp::invokeXObject(const std::string& name, const PdfObj* res, const Mat& ctm, int depth) {
  if (depth >= MAX_FORM_DEPTH || !res) return;
  // Heap scratch keeps recursion frames small (this nests up to 8 deep).
  auto xs = makeUniqueNoThrow<PdfObj[]>(3);
  if (!xs) return;
  const PdfObj* xmap = doc.resolve(res->find("XObject"), xs[0]);
  if (!xmap || !xmap->isDict()) return;
  const PdfObj* entry = xmap->find(name.c_str());
  const uint32_t objNum = entry && entry->kind == Kind::Ref ? entry->ref : 0;
  const PdfObj* form = doc.resolve(entry, xs[1]);
  if (!form || form->kind != Kind::Stream) return;
  const PdfObj* st = form->find("Subtype");
  if (st && st->isName("Image")) {
    if (!images || !images->emit) return;
    // The image is a block: close any open paragraph first, then let the sink
    // append its markup. If it emits nothing (unsupported or corrupt image),
    // undo the break so a skipped image leaves no stray paragraph split.
    const bool wasOpen = asmb.open;
    const size_t mark = asmb.out->size();
    asmb.finish();
    const size_t base = asmb.out->size();
    images->emit(images->ctx, doc, *form, objNum, res);
    if (asmb.out->size() == base) {
      asmb.out->resize(mark);
      asmb.open = wasOpen;
    }
    return;
  }
  if (!st || !st->isName("Form")) return;  // PostScript XObjects etc: ignored

  ByteBuf content;
  if (!doc.getStreamData(*form, content, PdfDoc::MAX_DECODED)) return;

  Mat fm;
  if (const PdfObj* mx = form->find("Matrix"); mx && mx->kind == Kind::Array && mx->arr.size() >= 6) {
    fm.a = numOf(mx->arr[0]);
    fm.b = numOf(mx->arr[1]);
    fm.c = numOf(mx->arr[2]);
    fm.d = numOf(mx->arr[3]);
    fm.e = numOf(mx->arr[4]);
    fm.f = numOf(mx->arr[5]);
  }
  const PdfObj* formRes = doc.resolve(form->find("Resources"), xs[2]);
  if (!formRes || !formRes->isDict()) formRes = res;  // fall back to caller's resources
  run(content.data(), content.size(), formRes, mul(fm, ctm), depth + 1);
}

bool Interp::run(const uint8_t* data, size_t len, const PdfObj* res, const Mat& ctm0, int depth) {
  if (depth >= MAX_FORM_DEPTH || len == 0) return true;
  PdfLexer lx(data, len);
  std::vector<PdfObj> stack;
  stack.reserve(8);
  std::vector<Mat> gsStack;
  Mat ctm = ctm0;
  Mat tm, tlm;
  const PdfFont* font = nullptr;
  float fontSize = 0;
  float leading = 0;

  auto opNum = [&](size_t fromEnd) -> float {
    return stack.size() > fromEnd ? numOf(stack[stack.size() - 1 - fromEnd]) : 0.0f;
  };
  auto doTd = [&](float tx, float ty) {
    Mat t;
    t.e = tx;
    t.f = ty;
    tlm = mul(t, tlm);
    tm = tlm;
  };
  auto doShow = [&](const std::string& bytes, std::string& utf8, size_t& glyphs) {
    if (bytes.empty()) return;
    if (font) {
      glyphs += bytes.size() / font->codeWidth();
      if (!font->decode((const uint8_t*)bytes.data(), bytes.size(), utf8)) lostShows++;
    } else {
      // No Tf seen (malformed content): latin1 passthrough beats losing text.
      glyphs += bytes.size();
      for (const char ch : bytes) {
        const uint8_t u = (uint8_t)ch;
        if (u >= 0x20) pdfAppendUtf8(utf8, u);
      }
    }
  };
  auto emitShow = [&](std::string& utf8, size_t glyphs) {
    totalShows++;
    const Mat m = mul(tm, ctm);
    const float det = m.a * m.d - m.b * m.c;
    float scale = sqrtf(fabsf(det));
    if (scale < 0.01f || scale > 100.0f) scale = 1.0f;
    const float es = (fontSize > 0.01f ? fontSize : 12.0f) * scale;
    asmb.show(m.e, m.f, es, utf8, glyphs);
  };

  // Bounded: every iteration either consumes >= 1 byte or exits.
  while (!lx.atEnd()) {
    lx.skipWs();
    if (lx.atEnd()) break;
    const size_t before = lx.pos();
    const uint8_t c = lx.peek();

    if (c == '/' || c == '(' || c == '<' || c == '[' || c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
      PdfObj o;
      if (lx.parseObject(o)) {
        if (stack.size() < MAX_OPERANDS) stack.push_back(std::move(o));
        continue;
      }
      lx.setPos(before + 1);  // resync on junk
      continue;
    }

    char op[24];
    if (!lx.readOperator(op)) {
      lx.setPos(before + 1);
      continue;
    }

    if (strcmp(op, "q") == 0) {
      if (gsStack.size() < MAX_GS_STACK) gsStack.push_back(ctm);
    } else if (strcmp(op, "Q") == 0) {
      if (!gsStack.empty()) {
        ctm = gsStack.back();
        gsStack.pop_back();
      }
    } else if (strcmp(op, "cm") == 0) {
      if (stack.size() >= 6) {
        Mat m;
        m.a = opNum(5);
        m.b = opNum(4);
        m.c = opNum(3);
        m.d = opNum(2);
        m.e = opNum(1);
        m.f = opNum(0);
        ctm = mul(m, ctm);
      }
    } else if (strcmp(op, "BT") == 0) {
      tm = Mat();
      tlm = Mat();
    } else if (strcmp(op, "ET") == 0) {
      // nothing to do
    } else if (strcmp(op, "Tf") == 0) {
      if (stack.size() >= 2 && stack[stack.size() - 2].kind == Kind::Name) {
        font = lookupFont(res, stack[stack.size() - 2].str);
        fontSize = opNum(0);
      }
    } else if (strcmp(op, "Td") == 0) {
      doTd(opNum(1), opNum(0));
    } else if (strcmp(op, "TD") == 0) {
      leading = -opNum(0);
      doTd(opNum(1), opNum(0));
    } else if (strcmp(op, "TL") == 0) {
      leading = opNum(0);
    } else if (strcmp(op, "T*") == 0) {
      doTd(0, -leading);
    } else if (strcmp(op, "Tm") == 0) {
      if (stack.size() >= 6) {
        tm.a = opNum(5);
        tm.b = opNum(4);
        tm.c = opNum(3);
        tm.d = opNum(2);
        tm.e = opNum(1);
        tm.f = opNum(0);
        tlm = tm;
      }
    } else if (strcmp(op, "Tj") == 0) {
      if (!stack.empty() && stack.back().kind == Kind::String) {
        std::string utf8;
        size_t glyphs = 0;
        doShow(stack.back().str, utf8, glyphs);
        emitShow(utf8, glyphs);
      }
    } else if (strcmp(op, "'") == 0) {
      if (!stack.empty() && stack.back().kind == Kind::String) {
        doTd(0, -leading);
        std::string utf8;
        size_t glyphs = 0;
        doShow(stack.back().str, utf8, glyphs);
        emitShow(utf8, glyphs);
      }
    } else if (strcmp(op, "\"") == 0) {
      if (stack.size() >= 3 && stack.back().kind == Kind::String) {
        doTd(0, -leading);  // aw/ac (word/char spacing) unused by the heuristics
        std::string utf8;
        size_t glyphs = 0;
        doShow(stack.back().str, utf8, glyphs);
        emitShow(utf8, glyphs);
      }
    } else if (strcmp(op, "TJ") == 0) {
      if (!stack.empty() && stack.back().kind == Kind::Array) {
        std::string utf8;
        size_t glyphs = 0;
        for (const auto& el : stack.back().arr) {
          if (el.kind == Kind::String) {
            doShow(el.str, utf8, glyphs);
          } else if (el.isNum() && (float)el.num < TJ_SPACE_GAP) {
            if (!utf8.empty() && utf8.back() != ' ') utf8 += ' ';
            glyphs++;
          }
        }
        emitShow(utf8, glyphs);
      }
    } else if (strcmp(op, "Do") == 0) {
      if (!stack.empty() && stack.back().kind == Kind::Name) {
        invokeXObject(stack.back().str, res, ctm, depth);
      }
    } else if (strcmp(op, "BI") == 0) {
      // Inline image: skipped, not extracted. Finding the true EI inside binary
      // sample data is guesswork, and inline images are near-always 1x1 masks
      // or rules rather than illustrations.
      // ponytail: extract them if a real book turns up that needs it.
      static bool inlineNoted = false;
      if (!inlineNoted) {
        inlineNoted = true;
        LOG_ERR("PDF", "image: inline images (BI/ID/EI) are skipped");
      }
      // Skip to whitespace-delimited EI.
      size_t q = lx.pos();
      while (q + 2 < len) {
        if (PdfLexer::isWs(data[q]) && data[q + 1] == 'E' && data[q + 2] == 'I' &&
            (q + 3 >= len || !PdfLexer::isRegular(data[q + 3]))) {
          q += 3;
          break;
        }
        q++;
      }
      lx.setPos(q + 2 >= len ? len : q);
    }
    // Tc/Tw/Tz/Ts and all path/color/marked-content operators: operands ignored.
    stack.clear();
  }
  return true;
}

}  // namespace

bool PdfText::extractPage(PdfDoc& doc, const PdfDoc::Page& page, PdfFontCache& fonts, std::string& out, bool* anyText,
                          const ImageSink* images) {
  if (anyText) *anyText = false;
  fonts.trim();

  PdfObj pageDict;
  if (!doc.loadObject(page.objNum, pageDict) || !pageDict.isDict()) return false;

  // Concatenate the page's content stream(s).
  ByteBuf content;
  PdfObj cStore;
  const PdfObj* contents = doc.resolve(pageDict.find("Contents"), cStore);
  if (!contents) return true;  // blank page
  auto appendStream = [&](const PdfObj& stm) {
    ByteBuf d;
    if (!doc.getStreamData(stm, d, PdfDoc::MAX_DECODED)) return;
    if (content.size() + d.size() + 1 > PdfDoc::MAX_DECODED) return;
    content.append(d.data(), d.size());
    const uint8_t nl = '\n';
    content.append(&nl, 1);
  };
  if (contents->kind == PdfObj::Kind::Stream) {
    appendStream(*contents);
  } else if (contents->kind == PdfObj::Kind::Array) {
    for (const auto& el : contents->arr) {
      PdfObj es;
      const PdfObj* s = doc.resolve(&el, es);
      if (s && s->kind == PdfObj::Kind::Stream) appendStream(*s);
    }
  }
  if (content.empty()) return true;

  PdfObj resStore;
  const PdfObj* res = doc.resolve(&page.resources, resStore);
  if (res && !res->isDict()) res = nullptr;

  Assembler asmb;
  asmb.out = &out;
  Interp interp{doc, fonts, asmb, images};
  Mat identity;
  interp.run(content.data(), content.size(), res, identity, 0);
  asmb.finish();

  if (interp.totalShows > 0 && interp.lostShows * 2 > interp.totalShows) {
    LOG_ERR("PDF", "page obj %u: %d/%d text ops undecodable (CID font without ToUnicode)", (unsigned)page.objNum,
            interp.lostShows, interp.totalShows);
  }
  if (anyText) *anyText = asmb.any;
  return true;
}
