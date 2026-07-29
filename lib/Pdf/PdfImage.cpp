#include "PdfImage.h"

#include <Logging.h>
#include <Memory.h>
#include <PngWriter.h>

#include <cstring>

using Kind = PdfObj::Kind;

namespace PdfImage {
namespace {

constexpr int MAX_CS_DEPTH = 4;
constexpr uint16_t PALETTE_ENTRIES = 256;  // always full: no index can be out of range

// Which of the unsupported formats has already been reported. Keyed by format,
// not by image, so a 300-page CCITT scan produces one log line, not 300.
enum class Skip : uint8_t { Jpx, Ccitt, Lzw, ChainedDct, Cmyk, Depth, ColorSpace, Mask, Count_ };
bool skipLogged[(size_t)Skip::Count_];

void logOnce(Skip s, const char* what) {
  if (skipLogged[(size_t)s]) return;
  skipLogged[(size_t)s] = true;
  LOG_ERR("PDF", "image: %s", what);
}

enum class CS : uint8_t { Gray, Rgb, Indexed };

struct Layout {
  CS space = CS::Gray;
  int comps = 1;  // source components per pixel
};

int32_t intField(PdfDoc& doc, const PdfObj& d, const char* key, int32_t def) {
  PdfObj store;
  const PdfObj* v = doc.resolve(d.find(key), store);
  return v && v->isNum() ? v->asInt() : def;
}

// Fills `palette` (PALETTE_ENTRIES * 3 bytes) from an /Indexed lookup table.
bool buildPalette(PdfDoc& doc, const PdfObj* lookup, const Layout& base, int32_t hival, uint8_t* palette) {
  if (hival < 0 || hival > 255) return false;
  ByteBuf streamBytes;
  const uint8_t* src = nullptr;
  size_t srcLen = 0;
  if (lookup->kind == Kind::String) {
    src = (const uint8_t*)lookup->str.data();
    srcLen = lookup->str.size();
  } else if (lookup->kind == Kind::Stream) {
    if (!doc.getStreamData(*lookup, streamBytes, 256u * 1024)) return false;
    src = streamBytes.data();
    srcLen = streamBytes.size();
  } else {
    return false;
  }
  const size_t bc = (size_t)base.comps;  // 1 (gray) or 3 (rgb)
  memset(palette, 0, (size_t)PALETTE_ENTRIES * 3);
  for (size_t i = 0; i <= (size_t)hival; i++) {
    if (srcLen / bc <= i) break;  // truncated table: remaining entries stay black
    const uint8_t* e = src + i * bc;
    if (bc == 3) {
      palette[i * 3 + 0] = e[0];
      palette[i * 3 + 1] = e[1];
      palette[i * 3 + 2] = e[2];
    } else {
      palette[i * 3 + 0] = palette[i * 3 + 1] = palette[i * 3 + 2] = e[0];
    }
  }
  return true;
}

// Classifies a colour space object into the three PNG shapes we can emit.
// Recurses for /ICCBased alternates, /Indexed bases and names that resolve
// through the resource dict's /ColorSpace map.
bool classify(PdfDoc& doc, const PdfObj* cs, const PdfObj* res, int depth, Layout& out, uint8_t* palette) {
  if (!cs || depth > MAX_CS_DEPTH) return false;

  if (cs->kind == Kind::Name) {
    const std::string& n = cs->str;
    if (n == "DeviceGray" || n == "G" || n == "CalGray") {
      out = {CS::Gray, 1};
      return true;
    }
    if (n == "DeviceRGB" || n == "RGB" || n == "CalRGB") {
      out = {CS::Rgb, 3};
      return true;
    }
    if (n == "DeviceCMYK" || n == "CMYK") {
      logOnce(Skip::Cmyk, "/DeviceCMYK not supported, skipped");
      return false;
    }
    // A resource-dict colour space name (/CS0 -> [/ICCBased 12 0 R] etc).
    if (!res) return false;
    PdfObj mapStore, entryStore;
    const PdfObj* map = doc.resolve(res->find("ColorSpace"), mapStore);
    if (!map || !map->isDict()) return false;
    const PdfObj* entry = doc.resolve(map->find(n.c_str()), entryStore);
    if (!entry) return false;
    return classify(doc, entry, res, depth + 1, out, palette);
  }

  if (cs->kind != Kind::Array || cs->arr.empty()) return false;
  PdfObj famStore;
  const PdfObj* fam = doc.resolve(&cs->arr[0], famStore);
  if (!fam || fam->kind != Kind::Name) return false;
  const std::string family = fam->str;  // copied: famStore is reused below

  if (cs->arr.size() == 1) {  // [/DeviceGray] and friends
    PdfObj wrap;
    wrap.kind = Kind::Name;
    wrap.str = family;
    return classify(doc, &wrap, res, depth + 1, out, palette);
  }

  if (family == "ICCBased") {
    PdfObj store;
    const PdfObj* stm = doc.resolve(&cs->arr[1], store);
    if (!stm || !stm->isDict()) return false;
    const int32_t n = intField(doc, *stm, "N", 0);
    if (n == 1) {
      out = {CS::Gray, 1};
      return true;
    }
    if (n == 3) {
      out = {CS::Rgb, 3};
      return true;
    }
    if (n == 4) logOnce(Skip::Cmyk, "/ICCBased 4-component (CMYK) not supported, skipped");
    return false;
  }

  if (family == "Indexed" || family == "I") {
    if (cs->arr.size() < 4 || !palette) return false;
    PdfObj baseStore;
    const PdfObj* base = doc.resolve(&cs->arr[1], baseStore);
    Layout baseLay;
    if (!classify(doc, base, res, depth + 1, baseLay, nullptr)) return false;
    if (baseLay.space == CS::Indexed) return false;  // /Indexed of /Indexed: nonsense
    PdfObj hiStore, lutStore;
    const PdfObj* hi = doc.resolve(&cs->arr[2], hiStore);
    const PdfObj* lut = doc.resolve(&cs->arr[3], lutStore);
    if (!hi || !hi->isNum() || !lut) return false;
    if (!buildPalette(doc, lut, baseLay, hi->asInt(), palette)) return false;
    out = {CS::Indexed, 1};
    return true;
  }

  if (family == "DeviceGray" || family == "CalGray") {
    out = {CS::Gray, 1};
    return true;
  }
  if (family == "DeviceRGB" || family == "CalRGB") {
    out = {CS::Rgb, 3};
    return true;
  }
  if (family == "DeviceCMYK") {
    logOnce(Skip::Cmyk, "/DeviceCMYK not supported, skipped");
    return false;
  }
  logOnce(Skip::ColorSpace, "colour space not supported (Lab/Separation/DeviceN/Pattern), skipped");
  return false;
}

// Collects the /Filter chain names (PdfDoc supports at most 4 stages).
int collectFilters(PdfDoc& doc, const PdfObj& img, std::string names[4]) {
  PdfObj store;
  const PdfObj* f = doc.resolve(img.find("Filter"), store);
  if (!f) return 0;
  if (f->kind == Kind::Name) {
    names[0] = f->str;
    return 1;
  }
  if (f->kind != Kind::Array) return 0;
  int n = 0;
  for (const auto& el : f->arr) {
    if (n >= 4) break;
    PdfObj es;  // separate storage, so `store` (and therefore `f`) stays valid
    const PdfObj* nm = doc.resolve(&el, es);
    if (!nm || nm->kind != Kind::Name) return 0;  // unreadable chain: treat as raw
    names[n++] = nm->str;
  }
  return n;
}

bool isDct(const std::string& n) { return n == "DCTDecode" || n == "DCT"; }

}  // namespace

void resetSkipLog() { memset(skipLogged, 0, sizeof(skipLogged)); }

bool decode(PdfDoc& doc, const PdfObj& img, const PdfObj* res, Decoded& out) {
  out.jpeg.clear();
  out.png.clear();
  out.width = out.height = 0;
  if (img.kind != Kind::Stream) return false;

  const int32_t wI = intField(doc, img, "Width", 0);
  const int32_t hI = intField(doc, img, "Height", 0);
  if (wI <= 0 || hI <= 0 || wI > 65535 || hI > 65535) return false;
  const uint64_t w = (uint64_t)wI;
  const uint64_t h = (uint64_t)hI;
  if (w * h > MAX_PIXELS) {  // both <= 65535, so the product cannot overflow
    LOG_ERR("PDF", "image %dx%d exceeds %u megapixels, skipped", (int)wI, (int)hI,
            (unsigned)(MAX_PIXELS / (1024 * 1024)));
    return false;
  }

  std::string filters[4];
  const int nf = collectFilters(doc, img, filters);

  // JPEG: the encoded stream *is* the wanted output. Only when DCTDecode is the
  // whole chain — behind an ASCII85/Flate stage we would have to decode it.
  if (nf >= 1 && isDct(filters[nf - 1])) {
    if (nf > 1) {
      logOnce(Skip::ChainedDct, "/DCTDecode behind another filter not supported, skipped");
      return false;
    }
    if (!doc.getRawStream(img, out.jpeg, (size_t)MAX_SAMPLE_BYTES)) {
      LOG_ERR("PDF", "image: JPEG stream unreadable, skipped");
      return false;
    }
    if (out.jpeg.size() < 4 || out.jpeg.data()[0] != 0xFF || out.jpeg.data()[1] != 0xD8) {
      LOG_ERR("PDF", "image: /DCTDecode stream is not a JPEG, skipped");
      out.jpeg.clear();
      return false;
    }
    out.width = (uint32_t)w;
    out.height = (uint32_t)h;
    return true;
  }
  for (int i = 0; i < nf; i++) {
    if (filters[i] == "JPXDecode") {
      logOnce(Skip::Jpx, "/JPXDecode (JPEG 2000) not supported, skipped");
      return false;
    }
    if (filters[i] == "CCITTFaxDecode" || filters[i] == "CCF") {
      logOnce(Skip::Ccitt, "/CCITTFaxDecode not supported, skipped");
      return false;
    }
    if (filters[i] == "LZWDecode" || filters[i] == "LZW") {
      logOnce(Skip::Lzw, "/LZWDecode not supported, skipped");
      return false;
    }
    if (isDct(filters[i])) {
      logOnce(Skip::ChainedDct, "/DCTDecode behind another filter not supported, skipped");
      return false;
    }
  }

  // Raster path: raw samples (no filter, Flate, RunLength or ASCIIHex) -> PNG.
  bool imageMask = false;
  {
    PdfObj store;
    const PdfObj* m = doc.resolve(img.find("ImageMask"), store);
    if (m && m->kind == Kind::Bool) imageMask = m->boolVal;
  }
  const int32_t bpc = imageMask ? 1 : intField(doc, img, "BitsPerComponent", 8);
  if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8) {
    logOnce(Skip::Depth, "/BitsPerComponent other than 1/2/4/8 not supported, skipped");
    return false;
  }

  // 768 bytes: too big for the stack budget, and only /Indexed needs it.
  std::unique_ptr<uint8_t[]> palette;
  Layout lay{CS::Gray, 1};
  if (imageMask) {
    // A stencil mask paints where the sample is 0, so it rasterises exactly as
    // a 1-bit greyscale image: 0 -> black, 1 -> white (unpainted paper).
    lay = {CS::Gray, 1};
  } else {
    palette = makeUniqueNoThrow<uint8_t[]>((size_t)PALETTE_ENTRIES * 3);
    if (!palette) {
      LOG_ERR("PDF", "OOM: image palette");
      return false;
    }
    PdfObj csStore;
    const PdfObj* cs = doc.resolve(img.find("ColorSpace"), csStore);
    if (!cs || cs->kind == Kind::Null) {
      lay = {CS::Gray, 1};  // absent /ColorSpace: assume grey rather than drop
    } else if (!classify(doc, cs, res, 0, lay, palette.get())) {
      return false;  // classify() logged the reason
    }
    if (lay.space != CS::Indexed) palette.reset();
  }

  if (img.find("SMask") || img.find("Mask")) {
    logOnce(Skip::Mask, "/SMask or /Mask transparency ignored, image drawn opaque");
  }

  const uint64_t srcRowBytes = (w * (uint64_t)lay.comps * (uint64_t)bpc + 7) / 8;
  const uint64_t outRowBytes = w * (lay.space == CS::Rgb ? 3u : 1u);
  const uint64_t outTotal = outRowBytes * h;
  if (srcRowBytes == 0 || outTotal == 0 || outTotal > MAX_SAMPLE_BYTES) {
    LOG_ERR("PDF", "image %dx%d needs %llu sample bytes (cap %llu), skipped", (int)wI, (int)hI,
            (unsigned long long)outTotal, (unsigned long long)MAX_SAMPLE_BYTES);
    return false;
  }

  ByteBuf samples;
  if (!doc.getStreamData(img, samples, (size_t)MAX_SAMPLE_BYTES)) {
    LOG_ERR("PDF", "image: sample stream undecodable, skipped");
    return false;
  }
  // Division form: no product to overflow, and no `a + b > len` comparison.
  if (samples.size() / srcRowBytes < h) {
    LOG_ERR("PDF", "image %dx%d truncated (%u bytes, need %llu), skipped", (int)wI, (int)hI, (unsigned)samples.size(),
            (unsigned long long)(srcRowBytes * h));
    return false;
  }

  // Sub-byte depths expand into their own buffer; 8-bit samples are already the
  // PNG row layout, so the decoded stream doubles as the raster.
  std::unique_ptr<uint8_t[]> expanded;
  uint8_t* raster = samples.data();
  if (bpc != 8) {
    expanded = makeUniqueNoThrow<uint8_t[]>((size_t)outTotal);
    if (!expanded) {
      LOG_ERR("PDF", "OOM: %llu byte raster", (unsigned long long)outTotal);
      return false;
    }
    const uint32_t mask = (1u << bpc) - 1u;
    // Indices must survive verbatim; intensities scale to the full 0..255 range
    // (1-bit x255, 2-bit x85, 4-bit x17).
    const uint32_t scale = lay.space == CS::Indexed ? 1u : 255u / mask;
    const uint64_t perRow = w * (uint64_t)lay.comps;
    for (uint64_t y = 0; y < h; y++) {
      const uint8_t* src = samples.data() + y * srcRowBytes;
      uint8_t* dst = expanded.get() + y * outRowBytes;
      uint32_t bit = 0;
      for (uint64_t i = 0; i < perRow; i++) {
        const uint32_t v = ((uint32_t)src[bit >> 3] >> (8u - (uint32_t)bpc - (bit & 7u))) & mask;
        dst[i] = (uint8_t)(v * scale);
        bit += (uint32_t)bpc;
      }
    }
    raster = expanded.get();
  }

  // /Decode [1 0 ...]: inverted intensity range. Very common on bilevel scans
  // and on stencil masks that paint where the bit is set.
  if (lay.space != CS::Indexed) {
    PdfObj store;
    const PdfObj* dec = doc.resolve(img.find("Decode"), store);
    if (dec && dec->kind == Kind::Array && dec->arr.size() >= 2 && dec->arr[0].isNum() && dec->arr[1].isNum() &&
        dec->arr[0].num > dec->arr[1].num) {
      for (uint64_t i = 0; i < outTotal; i++) raster[i] = (uint8_t)~raster[i];
    }
  }

  // png::encode builds its output in std::vectors, which abort() on OOM (no
  // exceptions). Probe the worst case (raw scanlines + encoded copy) nothrow
  // first so an oversized illustration is skipped instead.
  const uint64_t needed = (outTotal + h) * 2 + 4096;  // <= ~16MB by the caps above
  if (!makeUniqueNoThrow<uint8_t[]>((size_t)needed)) {
    LOG_ERR("PDF", "image %dx%d too large to re-encode (%llu bytes), skipped", (int)wI, (int)hI,
            (unsigned long long)needed);
    return false;
  }
  const png::Color color = lay.space == CS::Rgb    ? png::Color::Rgb8
                           : lay.space == CS::Gray ? png::Color::Gray8
                                                   : png::Color::Palette8;
  png::Palette pal;
  if (lay.space == CS::Indexed) {
    pal.rgb = palette.get();
    pal.count = PALETTE_ENTRIES;
  }
  if (!png::encode(raster, (uint32_t)w, (uint32_t)h, color, pal, out.png)) {
    LOG_ERR("PDF", "image %dx%d PNG re-encode failed, skipped", (int)wI, (int)hI);
    out.png.clear();
    return false;
  }
  out.width = (uint32_t)w;
  out.height = (uint32_t)h;
  return true;
}

}  // namespace PdfImage
