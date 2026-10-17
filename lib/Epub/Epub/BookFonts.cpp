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
// The state file: kVersion, then a code -- None, Family (then u8 nameLen and the name), or kFailed + n for n
// failed attempts so far. At kMaxAttempts the book is given up on, as None.
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
// accents-only mode"), reworked for this CSS resolver (tag, class and id; no ancestor selectors) and for one
// answer per book over several chapters. Text in div, li and dd paragraphs counts like p, alignment is
// judged where the text is, and a frame cap of 24 rather than 16.
class BodyFontProbe {
 public:
  explicit BodyFontProbe(CssParser& css) : css_(css) {}

  // A chapter: the element frames start over; the counts carry on.
  void beginParser(SaxParser& parser) {
    parser_ = &parser;
    depth_ = 0;
  }

  // html or body's font-family; 0 when neither declares one (or declares a generic family).
  uint32_t rootFamily() const { return rootFamily_; }
  uint32_t textBytes() const {
    uint32_t total = 0;
    for (const Usage& u : usage_) total += u.bytes;
    return total;
  }
  uint32_t family() const {
    if (rootFamily_ != 0) return rootFamily_;
    const auto best = std::max_element(usage_.begin(), usage_.end(),
                                       [](const Usage& a, const Usage& b) { return a.bytes < b.bytes; });
    return best->family;
  }

  static void onStart(void* ctx, const char* name, const char** atts) {
    auto& self = *static_cast<BodyFontProbe*>(ctx);
    if (self.parser_->byteOffset() >= kProbeChapterBytes) {
      self.parser_->stop();
      return;
    }
    const size_t index = self.depth_++;
    if (index >= kMaxDepth) return;
    const char* classes = "";
    const char* id = "";
    const char* inlineStyle = nullptr;
    bool hidden = false;
    for (size_t i = 0; atts && atts[i] && atts[i + 1]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classes = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        id = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        inlineStyle = atts[i + 1];
      } else if (strcmp(atts[i], "hidden") == 0) {
        hidden = true;
      }
    }
    Frame& frame = self.frames_[index];
    frame = index == 0 ? Frame{} : self.frames_[index - 1];
    const bool body = strcasecmp(name, "body") == 0;
    const bool root = body || strcasecmp(name, "html") == 0;
    bool none = false;  // display: none, the inline style having the last word
    self.tag_.assign(name);
    self.class_.assign(classes);
    self.id_.assign(self.css_.hasIdSelectors() ? id : "");
    self.apply(frame, self.css_.resolveStyle(self.tag_, self.class_, self.id_), root, none);
    if (inlineStyle) {
      self.inline_.assign(inlineStyle);
      self.apply(frame, CssParser::parseInlineStyle(self.inline_), root, none);
    }
    frame.inBody = frame.inBody || body;
    frame.ignored = frame.ignored || hidden || none || isSetApart(name);
    if (isParagraph(name)) {
      frame.inParagraph = true;
      frame.paragraphFamily = frame.family;  // an inline accent inside does not change the paragraph's
    }
  }

  static void onEnd(void* ctx, const char*) {
    auto& self = *static_cast<BodyFontProbe*>(ctx);
    if (self.depth_ > 0) --self.depth_;
  }

  static void onText(void* ctx, const char* text, const int len) {
    auto& self = *static_cast<BodyFontProbe*>(ctx);
    if (self.parser_->byteOffset() >= kProbeChapterBytes) {
      self.parser_->stop();
      return;
    }
    if (self.depth_ == 0 || self.depth_ > kMaxDepth) return;
    const Frame& frame = self.frames_[self.depth_ - 1];
    // A centred or right-aligned paragraph is a title, a date, a signature: typography, not body text.
    if (!frame.inBody || !frame.inParagraph || frame.ignored || frame.alignment == CssTextAlign::Center ||
        frame.alignment == CssTextAlign::Right) {
      return;
    }
    const auto bytes = static_cast<uint32_t>(VisibleText::visibleBytes(text, len));
    if (bytes == 0) return;
    for (Usage& u : self.usage_) {
      if (u.bytes == 0 || u.family == frame.paragraphFamily) {
        u.family = frame.paragraphFamily;
        u.bytes += bytes;
        return;
      }
    }
  }

  // An entity the parser passes through (&nbsp; and the like) is one character of text.
  static void onDefault(void* ctx, const char* text, const int len) {
    if (len >= 3 && text[0] == '&' && text[len - 1] == ';') onText(ctx, "x", 1);
  }

 private:
  static constexpr size_t kMaxDepth = 24;
  static constexpr size_t kMaxFamilies = CssParser::MAX_FONT_FACES + 1;  // every face's family, and none
  struct Frame {
    uint32_t family = 0;           // font-family in effect, inherited
    uint32_t paragraphFamily = 0;  // the family of the paragraph the element is in
    CssTextAlign alignment = CssTextAlign::Justify;
    bool inBody = false;
    bool inParagraph = false;
    bool ignored = false;
  };
  struct Usage {
    uint32_t family = 0;
    uint32_t bytes = 0;
  };

  // What the probe takes from a style, in cascade order: the stylesheet's, then the style attribute's.
  void apply(Frame& frame, const CssStyle& style, const bool root, bool& none) {
    if (style.defined.fontFamily) {
      frame.family = style.fontFamily;
      if (root) rootFamily_ = style.fontFamily;
    }
    if (style.hasTextAlign()) frame.alignment = style.textAlign;
    if (style.hasDisplay()) none = style.display == CssDisplay::None;
    frame.ignored = frame.ignored || style.isElementHidden();
  }

  static bool isParagraph(const char* name) {
    return strcasecmp(name, "p") == 0 || strcasecmp(name, "div") == 0 || strcasecmp(name, "li") == 0 ||
           strcasecmp(name, "dd") == 0;
  }
  // Text whose font is the publisher's typography rather than the body's.
  static bool isSetApart(const char* name) {
    static constexpr const char* kTags[] = {"h1",  "h2",    "h3",     "h4",         "h5",         "h6",  "table",
                                            "nav", "aside", "figure", "figcaption", "blockquote", "pre", "rt"};
    for (const char* tag : kTags) {
      if (strcasecmp(name, tag) == 0) return true;
    }
    return VisibleText::isNonVisibleTag(name);
  }

  CssParser& css_;
  SaxParser* parser_ = nullptr;
  std::array<Frame, kMaxDepth> frames_{};
  std::array<Usage, kMaxFamilies> usage_{};
  size_t depth_ = 0;
  uint32_t rootFamily_ = 0;
  // resolveStyle()'s arguments, reused so an element costs no allocation once they have grown.
  std::string tag_, class_, id_, inline_;
};

// One SAX pass over an item of the book, its parser state in `scratch` when there is room. False when the item
// could not be read (or the parser not set up); a malformed document reports what it parsed and counts as read.
template <typename Handler>
bool parseItem(const Epub& epub, const std::string& href, Handler& handler, void (*onDefault)(void*, const char*, int),
               const bool htmlVoidTagRepair, void* state, BuildArena* scratch) {
  SaxParser parser;
  if (state) parser.setExternalState(state, SaxParser::stateBytes());
  if (!parser.init(&handler, &Handler::onStart, &Handler::onEnd, &Handler::onText, onDefault, htmlVoidTagRepair)) {
    LOG_ERR("BKF", "No memory for the parser");
    return false;
  }
  handler.beginParser(parser);
  SaxFeedSink sink(parser);
  if (!epub.readItemContentsToStream(href, sink, 1024, sink.stopFlag(), scratch)) {
    LOG_ERR("BKF", "Cannot read %s", href.c_str());
    return false;
  }
  return true;
}

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

enum class Kind : uint8_t { TrueType, Cff, Woff, Woff2, Collection, Unknown };

Kind classify(const uint8_t (&b)[4]) {
  const uint32_t tag = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                       (static_cast<uint32_t>(b[2]) << 8) | b[3];
  switch (tag) {
    case 0x00010000u:  // TrueType outlines
    case 0x74727565u:  // 'true' (Apple)
      return Kind::TrueType;
    case 0x4F54544Fu:  // 'OTTO': CFF outlines
      return Kind::Cff;
    case 0x774F4646u:  // 'wOFF'
      return Kind::Woff;
    case 0x774F4632u:  // 'wOF2'
      return Kind::Woff2;
    case 0x74746366u:  // 'ttcf'
      return Kind::Collection;
    default:
      return Kind::Unknown;
  }
}

const char* kindName(const Kind kind) {
  switch (kind) {
    case Kind::TrueType:
      return "TrueType";
    case Kind::Cff:
      return "CFF";
    case Kind::Woff:
      return "WOFF";
    case Kind::Woff2:
      return "WOFF2";
    case Kind::Collection:
      return "a collection";
    case Kind::Unknown:
      break;
  }
  return "encrypted";  // neither a font nor obfuscated with this book's identifiers
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

Kind xoredKind(const uint8_t (&sig)[4], const uint8_t* key) {
  const uint8_t b[4] = {static_cast<uint8_t>(sig[0] ^ key[0]), static_cast<uint8_t>(sig[1] ^ key[1]),
                        static_cast<uint8_t>(sig[2] ^ key[2]), static_cast<uint8_t>(sig[3] ^ key[3])};
  return classify(b);
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
  Kind kind = classify(sig);
  const uint8_t* key = nullptr;
  size_t keyLen = 0;
  size_t obfuscated = 0;
  if (kind == Kind::Unknown) {
    // An obfuscated font starts with its first bytes XORed; a key that gives a font's signature back is the
    // one. Testing the signature replaces reading META-INF/encryption.xml: an algorithm it names that is real
    // encryption gives no signature back, and the face is refused.
    if (!keys.loaded && !loadKeys(epub, scratch, keys)) return Copy::Failed;
    if (keys.hasIdpf && xoredKind(sig, keys.idpf) != Kind::Unknown) {
      key = keys.idpf;
      keyLen = sizeof(keys.idpf);
      obfuscated = kIdpfBytes;
    } else if (keys.hasAdobe && xoredKind(sig, keys.adobe) != Kind::Unknown) {
      key = keys.adobe;
      keyLen = sizeof(keys.adobe);
      obfuscated = kAdobeBytes;
    }
    if (key) kind = xoredKind(sig, key);
  }
  if (kind != Kind::TrueType) {
    LOG_INF("BKF", "%s is %s: not drawn", href.c_str(), kindName(kind));
    return Copy::Refused;
  }
  if (!epub.extractItemToFile(href, dest, scratch)) return Copy::Failed;
  if (key && !xorPrefix(dest, key, keyLen, obfuscated)) {
    Storage.remove(dest.c_str());
    return Copy::Failed;
  }
  LOG_DBG("BKF", "Copied %s%s", href.c_str(),
          key == nullptr ? "" : (obfuscated == kIdpfBytes ? " (IDPF)" : " (Adobe)"));
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

// Appends at most 40 characters of `text`, every one but [A-Za-z0-9-] as '_': a name any card takes.
void appendSafe(std::string& out, const std::string_view text) {
  for (const char c : text.substr(0, 40)) {
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    out += safe ? c : '_';
  }
}

// Where a face is copied: `<dir>/<style>-<the name of its file in the book>.ttf`.
std::string facePath(const std::string& dir, const uint8_t style, const std::string& href) {
  std::string_view file(href);
  file.remove_prefix(file.find_last_of('/') + 1);  // npos + 1 == 0: no folder
  std::string path = dir;
  path += '/';
  path += static_cast<char>('0' + style);
  path += '-';
  appendSafe(path, file.substr(0, file.find_last_of('.')));
  path += ".ttf";
  return path;
}

// The state file's code, and for a Family its name; kNoCode when there is no file of this kVersion.
constexpr uint8_t kNoCode = 0xFF;
uint8_t readCode(const std::string& cachePath, std::string* name) {
  const std::string path = cachePath + kStateFile;
  HalFile file;
  if (!Storage.exists(path.c_str()) || !Storage.openFileForRead("BKF", path, file)) return kNoCode;
  uint8_t head[3] = {};
  const int got = file.read(head, sizeof(head));
  uint8_t code = got >= 2 && head[0] == kVersion ? head[1] : kNoCode;
  if (code == kCodeFamily) {
    std::string declared(got == 3 ? head[2] : 0, '\0');
    if (declared.empty() || file.read(declared.data(), declared.size()) != static_cast<int>(declared.size())) {
      code = kNoCode;
    } else if (name) {
      *name = std::move(declared);
    }
  }
  file.close();
  return code;
}

uint8_t failedAttempts(const std::string& cachePath) {
  const uint8_t code = readCode(cachePath, nullptr);
  return code != kNoCode && code >= kCodeFailed ? static_cast<uint8_t>(code - kCodeFailed) : 0;
}

State writeState(const std::string& cachePath, const uint8_t code, const std::string_view name = {}) {
  Storage.mkdir((cachePath + kFontsDir).c_str());
  HalFile file;
  if (!Storage.openFileForWrite("BKF", cachePath + kStateFile, file)) return State::Pending;
  const uint8_t head[3] = {kVersion, code, static_cast<uint8_t>(std::min<size_t>(name.size(), 255))};
  bool ok = file.write(head, code == kCodeFamily ? 3 : 2) == (code == kCodeFamily ? 3u : 2u);
  if (code == kCodeFamily) ok = ok && file.write(name.data(), head[2]) == head[2];
  ok = file.close() && ok;
  if (!ok) {
    Storage.remove((cachePath + kStateFile).c_str());
    return State::Pending;
  }
  return BookFonts::read(cachePath);
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

State read(const std::string& cachePath, std::string* name) {
  const uint8_t code = readCode(cachePath, name);
  if (code == kCodeFamily) return State::Family;
  if (code == kCodeNone || (code != kNoCode && code >= kCodeFailed + kMaxAttempts)) return State::None;
  return State::Pending;
}

std::string familyDir(const std::string& cachePath, const std::string_view name) {
  std::string dir = cachePath + kFontsDir;
  dir += '/';
  appendSafe(dir, name.empty() ? std::string_view("font") : name);
  return dir;
}

uint32_t detectBodyFamily(const Epub& epub, BuildArena* scratch, bool* failed) {
  *failed = false;
  CssParser* css = epub.getCssParser();
  if (!css) return 0;
  // ~0.9 KB: the frames and the usage table. Not on the stack, which a section build shares.
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
  for (int i = first; i < count && i < first + kProbeChapters; ++i) {
    if (!parseItem(epub, epub.getSpineItem(i).href, *probe, &BodyFontProbe::onDefault, true, state.get(), scratch)) {
      *failed = true;
      break;
    }
    if (probe->rootFamily() != 0 || probe->textBytes() >= kProbeEnoughText) break;
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
  const uint8_t attempts = failedAttempts(cachePath);
  Storage.removeDir((cachePath + kFontsDir).c_str());  // whatever an earlier attempt left

  std::string records;
  if (!CssParser::loadFontFaces(cachePath, records)) return writeState(cachePath, kCodeNone);
  bool failed = false;
  const uint32_t body = detectBodyFamily(epub, scratch, &failed);
  if (failed) return writeState(cachePath, static_cast<uint8_t>(kCodeFailed + attempts + 1));
  CssParser::FontFace picked[4];
  pickFaces(records, body, picked);
  if (picked[0].name.empty()) {
    LOG_INF("BKF", "Body font not embedded");
    return writeState(cachePath, kCodeNone);
  }

  const std::string_view name = picked[0].name;
  const std::string dir = familyDir(cachePath, name);
  Storage.mkdir(dir.c_str());
  Keys keys;
  Copy regular = Copy::Refused;
  bool anyFailed = false;
  for (uint8_t style = 0; style < 4; ++style) {
    if (picked[style].name.empty()) continue;
    const std::string href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(std::string(picked[style].href)));
    const Copy copy = copyFace(epub, href, facePath(dir, style, href), keys, scratch);
    if (style == 0) regular = copy;
    anyFailed = anyFailed || copy == Copy::Failed;
    if (regular != Copy::Done) break;  // every other style falls back to regular
  }
  if (anyFailed) {
    Storage.removeDir(dir.c_str());
    return writeState(cachePath, static_cast<uint8_t>(kCodeFailed + attempts + 1));
  }
  if (regular != Copy::Done) {
    Storage.removeDir(dir.c_str());
    return writeState(cachePath, kCodeNone);
  }
  LOG_INF("BKF", "Body font: %s", dir.c_str());
  return writeState(cachePath, kCodeFamily, name);
}

}  // namespace BookFonts
