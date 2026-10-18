#include "BookFonts.h"

#include <BuildArena.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SaxParser/SaxFeedSink.h>
#include <SaxParser/SaxParser.h>
#include <strings.h>
#include <wolfssl/wolfcrypt/hash.h>  // wc_ShaHash: wolfCrypt is linked for TLS already

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../Epub.h"
#include "VisibleText.h"
#include "css/CssParser.h"
#include "parsers/ContentOpfParser.h"

namespace BookFonts {
namespace {

constexpr char kFontsDir[] = "/fonts";
constexpr char kStateFile[] = "/fonts/state.bin";
// The state file: kVersion, then a code -- None, Family (then the name, to the end of the file), or kFailed + n
// for n failed attempts so far. At kMaxAttempts the book is given up on, as None.
constexpr uint8_t kCodeNone = 0;
constexpr uint8_t kCodeFamily = 1;
constexpr uint8_t kCodeFailed = 2;
constexpr uint8_t kMaxAttempts = 3;

// The IDPF algorithm obfuscates a font's first 1040 bytes, Adobe's the first 1024 (EPUB OCF 3.0, section 4;
// Adobe's "Font Mangling").
constexpr size_t kIdpfBytes = 1040;
constexpr size_t kAdobeBytes = 1024;

// ---- Which family the body text is set in ----------------------------------------------------------------
//
// Based on crosspoint-reader PR #3934's BodyFontProbe (Julia Nguyen, "fix: preserve reader fonts in
// accents-only mode"), reworked for one answer per book over several chapters: the family most of the text is
// set in, an inline accent's text counting for the paragraph (p, div, li, dd) around it. Measured on 921 real
// books with @font-face rules, against the family each draws most of its text in with every chapter styled by
// the sheets it links: right for 839. #3934's further rules made it worse here, at 807: html's or body's
// family first (this parser merges a book's sheets, and a marketing page's `body` rule then names a family no
// chapter uses) and centred or right-aligned text passed over. Headings, tables and hidden text left out
// changed one book.
class BodyFontProbe {
 public:
  explicit BodyFontProbe(CssParser& css) : css_(css) {}

  // A chapter: the element frames start over; the counts carry on.
  void beginParser(SaxParser& parser) {
    parser_ = &parser;
    depth_ = 0;
  }

  uint32_t textBytes() const {
    uint32_t total = 0;
    for (const Usage& u : usage_) total += u.bytes;
    return total;
  }
  // The family with the most text; 0 (the reader's font) without any.
  uint32_t family() const {
    return std::max_element(usage_.begin(), usage_.end(),
                            [](const Usage& a, const Usage& b) { return a.bytes < b.bytes; })
        ->family;
  }

  static void onStart(void* ctx, const char* name, const char** atts) {
    auto& self = *static_cast<BodyFontProbe*>(ctx);
    if (self.stopAtLimit()) return;
    const size_t index = self.depth_++;
    if (index >= kMaxDepth) return;
    const char* classes = "";
    const char* id = "";
    const char* inlineStyle = nullptr;
    for (size_t i = 0; atts && atts[i] && atts[i + 1]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classes = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        id = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        inlineStyle = atts[i + 1];
      }
    }
    Frame& frame = self.frames_[index];
    frame = index == 0 ? Frame{} : self.frames_[index - 1];
    self.tag_.assign(name);
    self.class_.assign(classes);
    self.id_.assign(self.css_.hasIdSelectors() ? id : "");
    frame.take(self.css_.resolveStyle(self.tag_, self.class_, self.id_));
    if (inlineStyle) {  // the style attribute has the last word
      self.inline_.assign(inlineStyle);
      frame.take(CssParser::parseInlineStyle(self.inline_));
    }
    frame.inBody = frame.inBody || strcasecmp(name, "body") == 0;
    if (strcasecmp(name, "p") == 0 || strcasecmp(name, "div") == 0 || strcasecmp(name, "li") == 0 ||
        strcasecmp(name, "dd") == 0) {
      frame.paragraphFamily = frame.family;
      frame.inParagraph = true;
    }
  }

  static void onEnd(void* ctx, const char*) {
    auto& self = *static_cast<BodyFontProbe*>(ctx);
    if (self.depth_ > 0) --self.depth_;
  }

  static void onText(void* ctx, const char* text, const int len) {
    auto& self = *static_cast<BodyFontProbe*>(ctx);
    if (self.stopAtLimit() || self.depth_ == 0 || self.depth_ > kMaxDepth) return;
    const Frame& frame = self.frames_[self.depth_ - 1];
    const auto bytes = static_cast<uint32_t>(VisibleText::visibleBytes(text, len));
    if (!frame.inBody || bytes == 0) return;
    const uint32_t family = frame.inParagraph ? frame.paragraphFamily : frame.family;
    for (Usage& u : self.usage_) {
      if (u.bytes == 0 || u.family == family) {
        u.family = family;
        u.bytes += bytes;
        return;
      }
    }
  }

  // An entity the parser passes through (&nbsp; and the like) is one character of text. Without this handler
  // such an entity, undeclared in an XHTML file, would end the parse.
  static void onDefault(void* ctx, const char* text, const int len) {
    if (len >= 3 && text[0] == '&' && text[len - 1] == ';') onText(ctx, "x", 1);
  }

 private:
  static constexpr size_t kMaxDepth = 24;
  static constexpr size_t kMaxFamilies = CssParser::MAX_FONT_FACES + 1;  // every face's family, and none
  struct Frame {
    uint32_t family = 0;           // font-family in effect, inherited
    uint32_t paragraphFamily = 0;  // the family of the paragraph the element is in
    bool inBody = false;
    bool inParagraph = false;
    void take(const CssStyle& style) {
      if (style.defined.fontFamily) family = style.fontFamily;
    }
  };
  struct Usage {
    uint32_t family = 0;
    uint32_t bytes = 0;
  };

  // Past kProbeChapterBytes of the chapter: stops the parser.
  bool stopAtLimit() const {
    if (parser_->byteOffset() < kProbeChapterBytes) return false;
    parser_->stop();
    return true;
  }

  CssParser& css_;
  SaxParser* parser_ = nullptr;
  std::array<Frame, kMaxDepth> frames_{};
  std::array<Usage, kMaxFamilies> usage_{};
  size_t depth_ = 0;
  // resolveStyle()'s arguments, reused so an element costs no allocation once they have grown.
  std::string tag_, class_, id_, inline_;
};

// A block of `scratch` holding one parser state for the passes, released when it goes.
class ParserState {
 public:
  explicit ParserState(BuildArena* scratch) : scratch_(scratch) {
    if (!scratch_ || !scratch_->valid()) return;
    block_ = scratch_->reserveBlock();
    state_ = scratch_->alloc(SaxParser::stateBytes());
  }
  ~ParserState() {
    if (block_.valid()) scratch_->release(block_);
  }
  ParserState(const ParserState&) = delete;
  ParserState& operator=(const ParserState&) = delete;
  void* get() const { return state_; }

 private:
  BuildArena* scratch_;
  BuildArena::Block block_;
  void* state_ = nullptr;
};

// ---- The font files --------------------------------------------------------------------------------------

// Whether the font's first four bytes, XORed with `key`'s first four when it is set, are a TrueType signature
// (0x00010000, or 'true' from Apple). Anything else is not drawn: CFF outlines ('OTTO'), WOFF, WOFF2, a
// collection, or a font encrypted rather than obfuscated.
bool isTrueType(const uint8_t (&sig)[4], const uint8_t* key) {
  uint32_t tag = 0;
  for (int i = 0; i < 4; ++i) tag = tag << 8 | static_cast<uint8_t>(sig[i] ^ (key ? key[i] : 0));
  return tag == 0x00010000u || tag == 0x74727565u;
}

// The book's two obfuscation keys, read from the OPF the first time an obfuscated face needs them.
struct Keys {
  bool loaded = false;
  bool hasIdpf = false;
  bool hasAdobe = false;
  uint8_t idpf[20] = {};
  uint8_t adobe[16] = {};
};

bool loadKeys(const Epub& epub, BuildArena* scratch, Keys& keys) {
  keys.loaded = true;
  std::string opfPath;
  size_t opfSize = 0;
  if (!epub.contentOpfPath(&opfPath) || !epub.getItemSize(opfPath, &opfSize)) return false;
  // Released before the face is extracted, whose inflate ring needs the room; after the parser, which uses it.
  const ParserState state(scratch);
  // The package parser with no cache, as for a metadata sidecar: the metadata alone. About 1 KB, so on the heap
  // rather than in prepare()'s frame, under the probe and the copies. It holds the paths by reference.
  const std::string none;
  auto opf = makeUniqueNoThrow<ContentOpfParser>(none, none, opfSize, nullptr);
  if (!opf) {
    LOG_ERR("BKF", "OOM: package parser");
    return false;
  }
  if (state.get()) opf->setParserState(state.get());
  if (!opf->setup() || !epub.readItemContentsToStream(opfPath, *opf, 1024, opf->metadataRead(), scratch)) {
    LOG_ERR("BKF", "Cannot read %s", opfPath.c_str());
    return false;
  }
  keys.hasIdpf = idpfKey(opf->uniqueIdentifier, keys.idpf);
  keys.hasAdobe = adobeKey(opf->uuidIdentifier, keys.adobe);
  return true;
}

// XORs the first `count` bytes of the file at `path` with `key` (`keyLen` bytes, repeated), in place.
bool xorPrefix(const std::string& path, const uint8_t* key, const size_t keyLen, const size_t count) {
  HalFile file;
  if (!Storage.openFileForUpdate("BKF", path, file)) return false;
  uint8_t buf[64];
  bool ok = true;
  for (size_t at = 0; ok && at < count;) {
    const int got = file.read(buf, std::min(sizeof(buf), count - at));
    if (got <= 0) break;  // a font shorter than the obfuscated prefix
    for (int i = 0; i < got; ++i) buf[i] ^= key[(at + static_cast<size_t>(i)) % keyLen];
    ok = file.seekSet(at) && file.write(buf, static_cast<size_t>(got)) == static_cast<size_t>(got);
    at += static_cast<size_t>(got);
  }
  return file.close() && ok;
}

enum class Copy : uint8_t { Done, Refused, Failed };

// Copies one face to `dest` as plain TrueType, its obfuscation undone. Refused: not a font the TTF engine
// draws (logged). Failed: a read or a write went wrong, which may pass.
Copy copyFace(const Epub& epub, const std::string& href, const std::string& dest, Keys& keys, BuildArena* scratch) {
  size_t size = 0;
  if (!epub.getItemSize(href, &size) || size < 12 || size > kMaxFaceBytes) {
    LOG_INF("BKF", "%s: missing or %u bytes", href.c_str(), static_cast<unsigned>(size));
    return Copy::Refused;
  }
  uint8_t sig[4] = {};
  if (epub.readItemHeaderBytes(href, sig, sizeof(sig)) != sizeof(sig)) return Copy::Failed;
  const uint8_t* key = nullptr;
  size_t keyLen = 0;
  size_t obfuscated = 0;
  if (!isTrueType(sig, nullptr)) {
    // An obfuscated font starts with its first bytes XORed; a key that gives the signature back is the one.
    // Testing the signature replaces reading META-INF/encryption.xml: an algorithm it names that is real
    // encryption gives no signature back, and the face is refused.
    if (!keys.loaded && !loadKeys(epub, scratch, keys)) return Copy::Failed;
    if (keys.hasIdpf && isTrueType(sig, keys.idpf)) {
      key = keys.idpf;
      keyLen = sizeof(keys.idpf);
      obfuscated = kIdpfBytes;
    } else if (keys.hasAdobe && isTrueType(sig, keys.adobe)) {
      key = keys.adobe;
      keyLen = sizeof(keys.adobe);
      obfuscated = kAdobeBytes;
    } else {
      // The signature tells a support log what it is: 4f54544f 'OTTO' is CFF, 774f4646 WOFF, 774f4632 WOFF2.
      LOG_INF("BKF", "%s: not TrueType (%02x%02x%02x%02x)", href.c_str(), sig[0], sig[1], sig[2], sig[3]);
      return Copy::Refused;
    }
  }
  if (!epub.extractItemToFile(href, dest, scratch)) return Copy::Failed;
  if (key && !xorPrefix(dest, key, keyLen, obfuscated)) {
    Storage.remove(dest.c_str());
    return Copy::Failed;
  }
  return Copy::Done;
}

// For each style (EpdFontFamily numbering: bit 0 bold, bit 1 italic), the face of `family` nearest weight 400,
// or 700 for the bold styles; one with an empty name where the family has none.
void pickFaces(std::string_view records, const uint32_t family, CssParser::FontFace (&out)[4]) {
  for (CssParser::FontFace face; CssParser::nextFontFace(records, face);) {
    if (face.family != family) continue;
    const uint8_t style = (face.weight >= 600 ? 1 : 0) | (face.italic ? 2 : 0);
    const int target = (style & 1) != 0 ? 700 : 400;
    CssParser::FontFace& best = out[style];
    if (best.name.empty() || std::abs(face.weight - target) < std::abs(best.weight - target)) best = face;
  }
}

// The state file's code, and for a Family its name; kNoCode when there is no file of this kVersion.
constexpr uint8_t kNoCode = 0xFF;
uint8_t readCode(const std::string& cachePath, std::string* name) {
  const std::string path = cachePath + kStateFile;
  std::string bytes;
  // Checked first: an unexamined book has none, and a failed open logs.
  if (!Storage.exists(path.c_str()) || !Storage.readFileToString("BKF", path, 2 + 255, bytes) || bytes.size() < 2 ||
      bytes[0] != static_cast<char>(kVersion)) {
    return kNoCode;
  }
  const auto code = static_cast<uint8_t>(bytes[1]);
  if (code == kCodeFamily) {
    if (bytes.size() == 2) return kNoCode;
    bytes.erase(0, 2);
    if (name) *name = std::move(bytes);
  }
  return code;
}

State stateOf(const uint8_t code) {
  if (code == kCodeFamily) return State::Family;
  if (code == kCodeNone || (code != kNoCode && code >= kCodeFailed + kMaxAttempts)) return State::None;
  return State::Pending;
}

State writeState(const std::string& cachePath, const uint8_t code, const std::string_view name) {
  Storage.mkdir((cachePath + kFontsDir).c_str());
  std::string bytes{static_cast<char>(kVersion), static_cast<char>(code)};
  if (code == kCodeFamily) bytes.append(name.data(), std::min<size_t>(name.size(), 255));
  const std::string path = cachePath + kStateFile;
  HalFile file;
  if (!Storage.openFileForWrite("BKF", path, file)) return State::Pending;
  const bool written = file.write(bytes.data(), bytes.size()) == bytes.size();
  if (!file.close() || !written) {
    Storage.remove(path.c_str());
    return State::Pending;
  }
  return stateOf(code);
}

// Finds the body family and copies its faces out of the book; the state file's code for what came of it
// (kCodeFailed for a failed read), and for a Family `*name`, the name it declares (into `records`).
uint8_t examine(const Epub& epub, BuildArena* scratch, std::string& records, std::string_view& name) {
  if (!CssParser::loadFontFaces(epub.getCachePath(), records)) return kCodeNone;
  bool failed = false;
  const uint32_t body = detectBodyFamily(epub, scratch, &failed);
  if (failed) return kCodeFailed;
  CssParser::FontFace picked[4];
  pickFaces(records, body, picked);
  if (picked[0].name.empty()) {
    LOG_INF("BKF", "Body font not embedded");
    return kCodeNone;
  }

  const std::string dir = familyDir(epub.getCachePath());
  Storage.mkdir(dir.c_str());
  std::string dest = dir + "/0.ttf";  // the style's digit goes in place of the 0
  Keys keys;
  for (uint8_t style = 0; style < 4; ++style) {
    if (picked[style].name.empty()) continue;
    dest[dest.size() - 5] = static_cast<char>('0' + style);
    const std::string href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(std::string(picked[style].href)));
    const Copy copy = copyFace(epub, href, dest, keys, scratch);
    // A style refused falls back to regular; regular refused, the book keeps the reader's font.
    if (copy == Copy::Failed || (style == 0 && copy == Copy::Refused)) {
      Storage.removeDir(dir.c_str());
      return copy == Copy::Failed ? kCodeFailed : kCodeNone;
    }
  }
  name = picked[0].name;
  LOG_INF("BKF", "Body font %.*s", static_cast<int>(name.size()), name.data());
  return kCodeFamily;
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

// The keys and the obfuscation follow our web Font Manager's extraction (src/network/html/js/epub-fonts.js),
// which decodes the same two algorithms in the browser.
bool idpfKey(const std::string_view uniqueIdentifier, uint8_t (&key)[20]) {
  std::string stripped;
  stripped.reserve(uniqueIdentifier.size());
  for (const char c : uniqueIdentifier) {
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') stripped += c;
  }
  return !stripped.empty() &&
         wc_ShaHash(reinterpret_cast<const byte*>(stripped.data()), static_cast<word32>(stripped.size()), key) == 0;
}

bool adobeKey(const std::string_view uuid, uint8_t (&key)[16]) {
  constexpr std::string_view kPrefix = "urn:uuid:";
  if (uuid.size() < kPrefix.size() || strncasecmp(uuid.data(), kPrefix.data(), kPrefix.size()) != 0) return false;
  size_t bytes = 0;
  int high = -1;
  for (const char c : uuid.substr(kPrefix.size())) {
    if (c == '-') continue;
    const int v = hexValue(c);
    if (v < 0 || bytes == sizeof(key)) return false;
    if (high < 0) {
      high = v;
    } else {
      key[bytes++] = static_cast<uint8_t>((high << 4) | v);
      high = -1;
    }
  }
  return bytes == sizeof(key) && high < 0;
}

State read(const std::string& cachePath, std::string* name) { return stateOf(readCode(cachePath, name)); }

std::string familyDir(const std::string& cachePath) { return cachePath + kFontsDir; }

uint32_t detectBodyFamily(const Epub& epub, BuildArena* scratch, bool* failed) {
  *failed = false;
  CssParser* css = epub.getCssParser();
  if (!css) return 0;
  // ~0.7 KB: the frames and the usage table. Not on the stack, which a section build shares.
  auto probe = makeUniqueNoThrow<BodyFontProbe>(*css);
  if (!probe) {
    LOG_ERR("BKF", "OOM: probe");
    *failed = true;
    return 0;
  }
  const ParserState state(scratch);
  css->resetResolveStats();
  const int count = epub.getSpineItemsCount();
  const int first = std::max(0, epub.getSpineIndexForTextReference());
  for (int i = first; i < count && i < first + kProbeChapters && probe->textBytes() < kProbeEnoughText; ++i) {
    // One SAX pass over the chapter, the parser state in `scratch` when there is room. A malformed document
    // reports what it parsed and counts as read.
    SaxParser parser;
    if (state.get()) parser.setExternalState(state.get(), SaxParser::stateBytes());
    SaxFeedSink sink(parser);
    const bool ready = parser.init(probe.get(), &BodyFontProbe::onStart, &BodyFontProbe::onEnd, &BodyFontProbe::onText,
                                   &BodyFontProbe::onDefault, /*htmlVoidTagRepair=*/true);
    if (ready) probe->beginParser(parser);
    if (!ready || !epub.readItemContentsToStream(epub.getSpineItem(i).href, sink, 1024, sink.stopFlag(), scratch)) {
      LOG_ERR("BKF", "Cannot read chapter %d", i);
      *failed = true;
      break;
    }
  }
  // Short of heap, the resolver skips the rules it would read from the card: the families found would be
  // wrong, so this counts as a failed attempt rather than an answer.
  if (css->getResolveStats().lowHeapSkips != 0 && !css->isArenaResident()) {
    LOG_ERR("BKF", "Low heap: the probe missed CSS rules");
    *failed = true;
  }
  css->clearCaches(/*evictEverything=*/true);  // the resolver's caches go back to the heap
  return probe->family();
}

// Copying a book's faces into its own cache, for the reader to load from there, follows CrossInk (uxjulia/CrossInk)
// commit dfd2f31c (Julia Nguyen), MIT; here they are extracted once, de-obfuscated, and loaded as a TTF family.
State prepare(const Epub& epub, BuildArena* scratch) {
  const std::string& cachePath = epub.getCachePath();
  // The CSS was not loaded, or did not compile: nothing to decide from yet.
  if (!epub.getCssParser() || !epub.getCssParser()->hasCache()) return State::Pending;
  const uint8_t last = readCode(cachePath, nullptr);
  const uint8_t attempts = last != kNoCode && last >= kCodeFailed ? static_cast<uint8_t>(last - kCodeFailed) : 0;
  Storage.removeDir((cachePath + kFontsDir).c_str());  // whatever an earlier attempt left

  std::string records;
  std::string_view name;
  const uint8_t code = examine(epub, scratch, records, name);
  return writeState(cachePath, code == kCodeFailed ? static_cast<uint8_t>(kCodeFailed + attempts + 1) : code, name);
}

}  // namespace BookFonts
