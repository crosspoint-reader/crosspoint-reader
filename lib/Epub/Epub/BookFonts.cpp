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
constexpr char kAccentDir[] = "/fonts/accent";
constexpr char kStateFile[] = "/fonts.bin";  // beside fonts/, which prepare() empties
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
//
// The same pass counts the blocks each family is set in, for the accent family: what #3934 (Justin Mitchell) draws
// in its "Ignore Body Font" mode, the book's families other than the body's, here one family by blocks, as the
// reader lays a block out in one family (ChapterHtmlSlimParser::resolveBlockFont).
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
  // The family with the most text (by identity: a family named with and without a generic fallback is one), with
  // the generic fallback most of that text names; 0 (the reader's font) without any.
  uint32_t family() const {
    const Usage* best = &usage_[0];
    uint32_t bestBytes = 0;
    for (const Usage& u : usage_) {
      const uint32_t identity = CssFontFamily::identity(u.family);
      uint32_t bytes = 0;
      for (const Usage& v : usage_) {
        if (CssFontFamily::identity(v.family) == identity) bytes += v.bytes;
      }
      if (bytes > bestBytes ||
          (bytes == bestBytes && identity == CssFontFamily::identity(best->family) && u.bytes > best->bytes)) {
        best = &u;
        bestBytes = bytes;
      }
    }
    return best->family;
  }
  // The accent family (Publisher Fonts' Accents): of the families blocks are set in, other than `body`'s and the
  // reader's, the one the most blocks are set in that `usable` takes; its identity, or 0 for none.
  template <typename Usable>
  uint32_t accent(const uint32_t body, Usable usable) const {
    uint32_t best = 0;
    uint32_t bestBlocks = 0;
    for (const Usage& u : blocks_) {
      if (u.bytes <= bestBlocks || u.family == 0 || u.family == CssFontFamily::identity(body) || !usable(u.family)) {
        continue;
      }
      best = u.family;
      bestBlocks = u.bytes;
    }
    return best;
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
    const bool paragraph = strcasecmp(name, "p") == 0 || strcasecmp(name, "div") == 0 || strcasecmp(name, "li") == 0 ||
                           strcasecmp(name, "dd") == 0;
    if (paragraph) {
      frame.paragraphFamily = frame.family;
      frame.inParagraph = true;
    }
    // A block, which the reader lays out in one family: its text's when all of it is in one, else its own.
    if (paragraph || strcasecmp(name, "blockquote") == 0 || strcasecmp(name, "pre") == 0 ||
        ((name[0] | 0x20) == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0')) {
      frame.block = static_cast<int8_t>(index);
      frame.textState = 0;
    }
  }

  static void onEnd(void* ctx, const char*) {
    auto& self = *static_cast<BodyFontProbe*>(ctx);
    if (self.depth_ == 0) return;
    const size_t index = --self.depth_;
    if (index >= kMaxDepth) return;
    const Frame& frame = self.frames_[index];
    if (frame.block != static_cast<int8_t>(index) || frame.textState == 0) return;
    count(self.blocks_, CssFontFamily::identity(frame.textState == 1 ? frame.textFamily : frame.family), 1);
  }

  static void onText(void* ctx, const char* text, const int len) {
    auto& self = *static_cast<BodyFontProbe*>(ctx);
    if (self.stopAtLimit() || self.depth_ == 0 || self.depth_ > kMaxDepth) return;
    const Frame& frame = self.frames_[self.depth_ - 1];
    const auto bytes = static_cast<uint32_t>(VisibleText::visibleBytes(text, len));
    if (!frame.inBody || bytes == 0) return;
    count(self.usage_, frame.inParagraph ? frame.paragraphFamily : frame.family, bytes);
    if (frame.block < 0) return;
    Frame& block = self.frames_[frame.block];
    if (block.textState == 0) {
      block.textFamily = frame.family;
      block.textState = 1;
    } else if (block.textFamily != frame.family) {
      block.textState = 2;
    }
  }

  // An entity the parser passes through (&nbsp; and the like) is one character of text. Without this handler
  // such an entity, undeclared in an XHTML file, would end the parse.
  static void onDefault(void* ctx, const char* text, const int len) {
    if (len >= 3 && text[0] == '&' && text[len - 1] == ';') onText(ctx, "x", 1);
  }

 private:
  static constexpr size_t kMaxDepth = 24;
  // Every face's family, and none; a family named with and without a generic fallback counts twice.
  static constexpr size_t kMaxFamilies = CssParser::MAX_FONT_FACES + 4;
  struct Frame {
    uint32_t family = 0;           // font-family in effect, inherited
    uint32_t paragraphFamily = 0;  // the family of the paragraph the element is in
    uint32_t textFamily = 0;       // a block's: the family of its text so far
    bool inBody = false;
    bool inParagraph = false;
    int8_t block = -1;      // the frame of the block the element is in
    uint8_t textState = 0;  // a block's: 0 no text yet, 1 all of it in textFamily, 2 in several families
    void take(const CssStyle& style) {
      if (style.defined.fontFamily) family = style.fontFamily;
    }
  };
  struct Usage {
    uint32_t family = 0;
    uint32_t bytes = 0;  // or blocks
  };
  using Usages = std::array<Usage, kMaxFamilies>;

  // Adds `n` to `family`'s count; a family past the table's room is not counted.
  static void count(Usages& usages, const uint32_t family, const uint32_t n) {
    for (Usage& u : usages) {
      if (u.bytes == 0 || u.family == family) {
        u.family = family;
        u.bytes += n;
        return;
      }
    }
  }

  // Past kProbeChapterBytes of the chapter: stops the parser.
  bool stopAtLimit() const {
    if (parser_->byteOffset() < kProbeChapterBytes) return false;
    parser_->stop();
    return true;
  }

  CssParser& css_;
  SaxParser* parser_ = nullptr;
  std::array<Frame, kMaxDepth> frames_{};
  Usages usage_{};   // text bytes by family, an inline accent's counting for its paragraph's
  Usages blocks_{};  // blocks by family identity
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
  if (opf && state.get()) opf->setParserState(state.get());
  if (!opf || !opf->setup() || !epub.readItemContentsToStream(opfPath, *opf, 1024, opf->metadataRead(), scratch)) {
    LOG_ERR("BKF", "Cannot read %s", opfPath.c_str());  // or no heap for the parser
    return false;
  }
  keys.hasIdpf = idpfKey(std::move(opf->uniqueIdentifier), keys.idpf);
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

// The state file's code, and a family's name (see read()); kNoCode when there is no file of this kVersion.
constexpr uint8_t kNoCode = 0xFF;
constexpr size_t kHeaderBytes = 2 + 2 * sizeof(uint32_t);  // version, code, the body and accent families
uint8_t readCode(const std::string& cachePath, std::string* name, Families* families, const bool body) {
  const std::string path = cachePath + kStateFile;
  std::string bytes;
  // Checked first: an unexamined book has none, and a failed open logs.
  if (!Storage.exists(path.c_str()) || !Storage.readFileToString("BKF", path, kHeaderBytes + 2 * 255 + 1, bytes) ||
      bytes.size() < kHeaderBytes || bytes[0] != static_cast<char>(kVersion)) {
    return kNoCode;
  }
  const auto code = static_cast<uint8_t>(bytes[1]);
  if (families) {
    std::memcpy(&families->body, &bytes[2], sizeof(uint32_t));
    std::memcpy(&families->accent, &bytes[2 + sizeof(uint32_t)], sizeof(uint32_t));
  }
  // The names: the body family's, then '\0' and the accent family's (c_str() ends either).
  bytes.erase(0, kHeaderBytes);
  size_t length = strlen(bytes.c_str());
  if (code == kCodeFamily && length == 0) return kNoCode;
  if (name) {
    if (!body || code != kCodeFamily) {
      bytes.erase(0, std::min(length + 1, bytes.size()));
      length = strlen(bytes.c_str());
    }
    bytes.resize(length);
    *name = std::move(bytes);
  }
  return code;
}

State stateOf(const uint8_t code) {
  if (code == kCodeFamily) return State::Family;
  if (code == kCodeNone || (code != kNoCode && code >= kCodeFailed + kMaxAttempts)) return State::None;
  return State::Pending;
}

State writeState(const std::string& cachePath, const uint8_t code, const uint32_t body, const uint32_t accent,
                 const std::string_view names) {
  std::string bytes(kHeaderBytes, '\0');
  bytes[0] = static_cast<char>(kVersion);
  bytes[1] = static_cast<char>(code);
  std::memcpy(&bytes[2], &body, sizeof(body));
  std::memcpy(&bytes[2 + sizeof(body)], &accent, sizeof(accent));
  bytes.append(names);
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

// Finds the body and accent families and copies their faces out of the book; the state file's code for what came
// of it (kCodeFailed for a failed read). `found` gets the families, `names` their names as the book declares them
// (the body's for a Family, then '\0' and the accent's when it was copied), into `records`.
uint8_t examine(const Epub& epub, BuildArena* scratch, std::string& records, Families& found, std::string& names) {
  // A book without faces is probed too: Accents needs its body's generic family (CssFontFamily).
  if (!CssParser::loadFontFaces(epub.getCachePath(), records)) records.clear();
  bool failed = false;
  const Found families = detectFamilies(epub, scratch, &failed, records);
  if (failed) return kCodeFailed;
  found.body = families.body;
  Keys keys;
  uint8_t code = kCodeNone;
  // The body family to fonts/, the accent family to fonts/accent/.
  for (const bool accent : {false, true}) {
    CssParser::FontFace picked[4];
    pickFaces(records, accent ? families.accent : CssFontFamily::identity(families.body), picked);
    if (picked[0].name.empty()) continue;  // not embedded, or no regular face
    const std::string dir = familyDir(epub.getCachePath(), accent);
    Storage.mkdir(dir.c_str());
    std::string dest = dir + "/0.ttf";  // the style's digit goes in place of the 0
    Copy copy = Copy::Done;
    for (uint8_t style = 0; style < 4 && copy != Copy::Failed; ++style) {
      if (picked[style].name.empty()) continue;
      dest[dest.size() - 5] = static_cast<char>('0' + style);
      const std::string href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(std::string(picked[style].href)));
      copy = copyFace(epub, href, dest, keys, scratch);
      // A style refused falls back to regular; regular refused, the family keeps the reader's font.
      if (copy == Copy::Refused && style != 0) copy = Copy::Done;
      if (copy == Copy::Refused) break;
    }
    if (copy != Copy::Done) {
      Storage.removeDir(dir.c_str());
      if (copy == Copy::Failed) return kCodeFailed;
      continue;
    }
    LOG_INF("BKF", "%s font %.*s", accent ? "Accent" : "Body", static_cast<int>(picked[0].name.size()),
            picked[0].name.data());
    if (accent) {
      found.accent = families.accent;
      names.push_back('\0');
    } else {
      code = kCodeFamily;
    }
    names.append(picked[0].name);
  }
  return code;
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
bool idpfKey(std::string uniqueIdentifier, uint8_t (&key)[20]) {
  size_t n = 0;  // the white space is squeezed out in place
  for (const char c : uniqueIdentifier) {
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') uniqueIdentifier[n++] = c;
  }
  return n != 0 && wc_ShaHash(reinterpret_cast<const byte*>(uniqueIdentifier.data()), static_cast<word32>(n), key) == 0;
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

State read(const std::string& cachePath, std::string* name, Families* families, const bool body) {
  return stateOf(readCode(cachePath, name, families, body));
}

std::string familyDir(const std::string& cachePath, const bool accent) {
  return cachePath + (accent ? kAccentDir : kFontsDir);
}

Found detectFamilies(const Epub& epub, BuildArena* scratch, bool* failed, const std::string_view records) {
  *failed = false;
  CssParser* css = epub.getCssParser();
  if (!css) return {};
  // ~1.3 KB: the frames and the two tables. Not on the stack, which a section build shares.
  auto probe = makeUniqueNoThrow<BodyFontProbe>(*css);
  if (!probe) {
    LOG_ERR("BKF", "OOM: probe");
    *failed = true;
    return {};
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
  Found found;
  found.body = probe->family();
  // The accent needs a regular face to be drawn (the registry's rule for a family).
  found.accent = probe->accent(found.body, [records](const uint32_t family) {
    CssParser::FontFace faces[4];
    pickFaces(records, family, faces);
    return !faces[0].name.empty();
  });
  return found;
}

// Copying a book's faces into its own cache, for the reader to load from there, follows CrossInk (uxjulia/CrossInk)
// commit dfd2f31c (Julia Nguyen), MIT; here they are extracted once, de-obfuscated, and loaded as a TTF family.
State prepare(const Epub& epub, BuildArena* scratch) {
  const std::string& cachePath = epub.getCachePath();
  // The CSS was not loaded, or did not compile: nothing to decide from yet.
  if (!epub.getCssParser() || !epub.getCssParser()->hasCache()) return State::Pending;
  const uint8_t last = readCode(cachePath, nullptr, nullptr, true);
  const uint8_t attempts = last != kNoCode && last >= kCodeFailed ? static_cast<uint8_t>(last - kCodeFailed) : 0;
  Storage.removeDir((cachePath + kFontsDir).c_str());  // whatever an earlier attempt left

  std::string records;
  Families found;
  std::string names;
  const uint8_t code = examine(epub, scratch, records, found, names);
  return writeState(cachePath, code == kCodeFailed ? static_cast<uint8_t>(kCodeFailed + attempts + 1) : code,
                    found.body, found.accent, names);
}

}  // namespace BookFonts
