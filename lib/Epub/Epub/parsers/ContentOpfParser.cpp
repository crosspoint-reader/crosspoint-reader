#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

#include "../BookMetadataCache.h"
#include "../HashUtils.h"
#include "../htmlEntities.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char MEDIA_TYPE_PAGEMAP[] = "application/oebps-page-map+xml";
constexpr char MEDIA_TYPE_IMAGE_PREFIX[] = "image/";
constexpr char itemCacheFile[] = "/.items.bin";
constexpr size_t MAX_DESCRIPTION_LENGTH = 1024;

inline bool isTag(const char* name, const char* plainTag, const char* opfTag) {
  return strcmp(name, plainTag) == 0 || strcmp(name, opfTag) == 0;
}

inline bool isPackageTag(const char* name) { return isTag(name, "package", "opf:package"); }
inline bool isMetadataTag(const char* name) { return isTag(name, "metadata", "opf:metadata"); }
inline bool isManifestTag(const char* name) { return isTag(name, "manifest", "opf:manifest"); }
inline bool isSpineTag(const char* name) { return isTag(name, "spine", "opf:spine"); }
inline bool isGuideTag(const char* name) { return isTag(name, "guide", "opf:guide"); }
inline bool isMetaTag(const char* name) { return isTag(name, "meta", "opf:meta"); }
inline bool isItemTag(const char* name) { return isTag(name, "item", "opf:item"); }
inline bool isItemRefTag(const char* name) { return isTag(name, "itemref", "opf:itemref"); }
inline bool isReferenceTag(const char* name) { return isTag(name, "reference", "opf:reference"); }

bool startsWithImageMediaType(const char* mediaType) {
  constexpr size_t prefixLen = sizeof(MEDIA_TYPE_IMAGE_PREFIX) - 1;
  for (size_t i = 0; i < prefixLen; ++i) {
    // A shorter string mismatches on its NUL before this reads past the end.
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(mediaType[i])));
    if (c != MEDIA_TYPE_IMAGE_PREFIX[i]) {
      return false;
    }
  }
  return true;
}

// Everything the manifest handler needs to know about a media-type, computed once per
// DISTINCT value: items are only ever routed by these classes, never by the raw string.
enum class MediaClass : uint8_t { Other = 0, Image, Ncx, Css, PageMap };

MediaClass classifyMediaType(const char* mediaType) {
  if (mediaType == nullptr || *mediaType == '\0') return MediaClass::Other;
  if (startsWithImageMediaType(mediaType)) return MediaClass::Image;
  if (strcmp(mediaType, MEDIA_TYPE_NCX) == 0) return MediaClass::Ncx;
  if (strcmp(mediaType, MEDIA_TYPE_CSS) == 0) return MediaClass::Css;
  if (strcmp(mediaType, MEDIA_TYPE_PAGEMAP) == 0) return MediaClass::PageMap;
  return MediaClass::Other;
}

// True when `word` appears as a whole space-separated token in `props` (the OPF
// `properties` attribute format). Pointer scan — no std::string construction.
bool hasPropertyWord(const char* props, const char* word) {
  if (props == nullptr || *props == '\0') return false;
  const size_t wordLen = strlen(word);
  const char* p = props;
  while ((p = strstr(p, word)) != nullptr) {
    const bool startsToken = (p == props) || (p[-1] == ' ');
    const char after = p[wordLen];
    if (startsToken && (after == '\0' || after == ' ')) return true;
    p += wordLen;
  }
  return false;
}

// Append the UTF-8 encoding of a Unicode code point.
void appendUtf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out += static_cast<char>(cp);
  } else if (cp <= 0x7FF) {
    out += static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
  } else if (cp <= 0xFFFF) {
    out += static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu));
    out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
  } else {
    out += static_cast<char>(0xF0u | ((cp >> 18) & 0x07u));
    out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
    out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
  }
}

// Resolve a numeric character reference (&#N; decimal or &#xN; hex) into UTF-8.
// `entity` points at the leading '&', `len` includes the trailing ';'. Returns
// false when the reference is malformed or out of range.
bool appendNumericEntity(std::string& out, const char* entity, size_t len) {
  // Shortest valid form is "&#0;" (len 4); entity[1] must be '#'.
  if (len < 4 || entity[1] != '#') {
    return false;
  }

  size_t p = 2;
  int base = 10;
  if (entity[p] == 'x' || entity[p] == 'X') {
    base = 16;
    ++p;
  }
  if (p >= len - 1) {  // no digits before ';'
    return false;
  }

  uint32_t cp = 0;
  for (; p < len - 1; ++p) {
    const char c = entity[p];
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (base == 16 && c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (base == 16 && c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      return false;
    }
    cp = cp * static_cast<uint32_t>(base) + static_cast<uint32_t>(digit);
    if (cp > 0x10FFFF) {  // beyond Unicode range — bail rather than emit garbage
      return false;
    }
  }

  appendUtf8(out, cp);
  return true;
}

bool appendHtmlEntity(std::string& out, const std::string& html, size_t& i) {
  const size_t semi = html.find(';', i + 1);
  if (semi == std::string::npos) {
    return false;
  }

  const size_t len = semi - i + 1;
  const char* entity = html.data() + i;

  // Numeric character references (&#8212; / &#x2014;) are not in the named-entity
  // table; decode them directly. yxml already decodes these when they appear raw,
  // but Calibre-style descriptions often double-escape them (&amp;#8212;), so the
  // literal form reaches here after the outer &amp; is resolved.
  if (len > 3 && entity[1] == '#') {
    if (appendNumericEntity(out, entity, len)) {
      i = semi;
      return true;
    }
    return false;
  }

  // Metadata descriptions should collapse these to normal spaces; the chapter
  // renderer keeps NBSP because it has layout semantics there.
  if ((len == 6 && std::memcmp(entity, "&nbsp;", len) == 0) || (len == 6 && std::memcmp(entity, "&ensp;", len) == 0) ||
      (len == 6 && std::memcmp(entity, "&emsp;", len) == 0) ||
      (len == 8 && std::memcmp(entity, "&thinsp;", len) == 0)) {
    out += ' ';
    i = semi;
    return true;
  }

  const char* resolved = lookupHtmlEntity(entity, len);
  if (resolved == nullptr) {
    return false;
  }

  out += resolved;
  i = semi;
  return true;
}

// Strip HTML tags and collapse whitespace from a description string.
// Expat already decodes XML entities (&lt; → <), so we see raw angle brackets.
std::string stripHtml(const std::string& html) {
  std::string result;
  result.reserve(html.size());
  bool inTag = false;
  for (size_t i = 0; i < html.size(); ++i) {
    const char c = html[i];
    if (c == '<') {
      // Only treat as a tag if immediately followed (no space skip) by a tag-like character
      const size_t j = i + 1;
      if (j < html.size() &&
          (isalpha(static_cast<unsigned char>(html[j])) || html[j] == '/' || html[j] == '!' || html[j] == '?')) {
        inTag = true;
        // Ensure words don't merge when a tag is removed
        if (!result.empty() && result.back() != ' ') result += ' ';
      } else {
        result += c;
      }
    } else if (c == '>') {
      if (inTag) {
        inTag = false;
      } else {
        result += c;
      }
    } else if (!inTag) {
      if (c == '&') {
        // Decode HTML entities not covered by XML/Expat.
        if (!appendHtmlEntity(result, html, i)) {
          result += c;
        }
      } else if (c == '\n' || c == '\r' || c == '\t') {
        if (!result.empty() && result.back() != ' ') result += ' ';
      } else {
        result += c;
      }
    }
  }
  // Collapse consecutive spaces and trim trailing whitespace
  std::string out;
  out.reserve(result.size());
  bool lastSpace = false;
  for (char c : result) {
    if (c == ' ') {
      if (!lastSpace && !out.empty()) {
        out += ' ';
        lastSpace = true;
      }
    } else {
      out += c;
      lastSpace = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string trim(const std::string& in) {
  size_t start = 0;
  while (start < in.size() && (in[start] == ' ' || in[start] == '\n' || in[start] == '\r' || in[start] == '\t')) {
    ++start;
  }
  size_t end = in.size();
  while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\n' || in[end - 1] == '\r' || in[end - 1] == '\t')) {
    --end;
  }
  return in.substr(start, end - start);
}

// Whether `s` holds `lowerWord` at `at`, whatever its case.
bool hasWordAt(const std::string& s, const size_t at, const char* lowerWord) {
  const size_t n = strlen(lowerWord);
  if (s.size() < at + n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (static_cast<char>(tolower(static_cast<unsigned char>(s[at + i]))) != lowerWord[i]) return false;
  }
  return true;
}

// Drops a "urn:<word>:" or "<word>" prefix, and the colons and spaces after it, from an identifier's
// value ("urn:isbn:978...", "ISBN: 978..."). True when there was one.
bool dropIdentifierPrefix(std::string& value, const char* word) {
  size_t at = hasWordAt(value, 0, "urn:") ? 4 : 0;
  if (!hasWordAt(value, at, word)) return false;
  at += strlen(word);
  while (at < value.size() && (value[at] == ':' || value[at] == ' ')) ++at;
  value.erase(0, at);
  return true;
}

// Appends no further than `cap` bytes in total: a creator field is a name, not a document.
void appendCapped(std::string& out, const char* s, const size_t len, const size_t cap) {
  if (out.size() >= cap) return;
  out.append(s, std::min(len, cap - out.size()));
}

// The MARC relator code for an author. Lowercase by definition, but generators are not always careful.
bool isAutCode(const std::string& role) {
  const std::string r = trim(role);
  return r.size() == 3 && std::tolower(static_cast<unsigned char>(r[0])) == 'a' &&
         std::tolower(static_cast<unsigned char>(r[1])) == 'u' && std::tolower(static_cast<unsigned char>(r[2])) == 't';
}

// No role at all means the creator is the author.
bool isAuthorRole(const std::string& role) { return trim(role).empty() || isAutCode(role); }
}  // namespace

ContentOpfParser::Creator* ContentOpfParser::beginCreator(const char** atts) {
  if (creatorCount_ >= MAX_CREATORS) return nullptr;
  if (!creators_) {
    creators_ = makeUniqueNoThrow<Creator[]>(MAX_CREATORS);
    // Out of memory: the display author still works, there is just no primary author.
    if (!creators_) return nullptr;
  }
  Creator& creator = creators_[creatorCount_++];
  for (int i = 0; atts[i]; i += 2) {
    const char* value = atts[i + 1];
    if (strcmp(atts[i], "id") == 0) {
      appendCapped(creator.id, value, strlen(value), MAX_CREATOR_FIELD);
    } else if (strcmp(atts[i], "opf:role") == 0) {
      appendCapped(creator.role, value, strlen(value), MAX_CREATOR_FIELD);
    } else if (strcmp(atts[i], "opf:file-as") == 0) {
      appendCapped(creator.fileAs, value, strlen(value), MAX_CREATOR_FIELD);
    }
  }
  return &creator;
}

// The field a <meta refines="#id" property="..."> sets, or null when `id` is not a creator's, the
// property is not one this parser keeps, or the refinement must not change it.
std::string* ContentOpfParser::creatorField(const char* id, const char* property) {
  const bool role = strcmp(property, "role") == 0;
  if (!role && strcmp(property, "file-as") != 0) return nullptr;
  for (uint8_t i = 0; i < creatorCount_; ++i) {
    Creator& creator = creators_[i];
    if (creator.id.empty() || creator.id != id) continue;
    if (!role) return &creator.fileAs;
    // EPUB 3 lets a creator hold several roles, and being credited as author is what counts: once a
    // creator has "aut", a further role (an author who also illustrated) must not take it away.
    return isAutCode(creator.role) ? nullptr : &creator.role;
  }
  return nullptr;
}

// Which dc:identifier is an ISBN and which an ASIN follows crosspoint-reader PR #3804 ("feat(kosync):
// add server profiles and extended metadata", Jadehawk / @jadehawk): the scheme names it (Calibre
// writes ISBN, and MOBI-ASIN or AMAZON), or the value's own "isbn"/"asin" prefix does. The first of
// each is kept.
void ContentOpfParser::recordIdentifier() {
  if (identifierTooLong_) return;
  std::string value = trim(identifierText_);
  if (identifierIsUnique_ || uniqueIdentifier.empty()) uniqueIdentifier = value;
  if (uuidIdentifier.empty() && hasWordAt(value, 0, "urn:uuid:")) uuidIdentifier = value;
  if (value.size() > MAX_ISBN_OR_ASIN) return;
  const bool isbnPrefix = dropIdentifierPrefix(value, "isbn");
  const bool asinPrefix = !isbnPrefix && dropIdentifierPrefix(value, "asin");
  if (value.empty()) return;
  if (identifierScheme_ == IdentifierScheme::Isbn || isbnPrefix) {
    if (isbn.empty()) isbn = std::move(value);
  } else if (identifierScheme_ == IdentifierScheme::Asin || asinPrefix) {
    if (asin.empty()) asin = std::move(value);
  }
}

void ContentOpfParser::pickPrimaryAuthor() {
  for (uint8_t i = 0; i < creatorCount_; ++i) {
    Creator& creator = creators_[i];
    if (creator.name.empty() || !isAuthorRole(creator.role)) continue;
    primaryAuthor = std::move(creator.name);
    authorSort = trim(creator.fileAs);
    break;
  }
  // The table has served its purpose. A first open carries this parser on through the manifest and
  // spine, where a large book needs every byte of heap, so nothing of it stays.
  creators_.reset();
  creatorCount_ = 0;
  std::string().swap(creatorText_);
}

ContentOpfParser::ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath,
                                   const size_t xmlSize, BookMetadataCache* cache)
    : cachePath(cachePath), baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}

bool ContentOpfParser::setup() {
  if (!saxParser_.init(this, startElement, endElement, characterData)) {
    LOG_DBG("COF", "Couldn't allocate memory for parser");
    return false;
  }
  return true;
}

// The item store is only opened when there is a cache to serve (see the manifest/spine branches in
// startElement), so a null-cache parse reaches the manifest/spine/guide end tags with a handle that
// was never opened. One place to ask that question, instead of four.
void ContentOpfParser::closeItemStore() {
  if (tempItemStore) {
    tempItemStore.close();
  }
}

ContentOpfParser::~ContentOpfParser() {
  closeItemStore();
  const auto itemCachePath = cachePath + itemCacheFile;
  if (Storage.exists(itemCachePath.c_str())) {
    Storage.remove(itemCachePath.c_str());
  }
}

size_t ContentOpfParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContentOpfParser::write(const uint8_t* buffer, const size_t size) {
  if (!saxParser_.isActive()) return 0;

  stats.writeCalls++;
  stats.bytesParsed += size;

  remainingSize -= std::min(size, remainingSize);
  if (!saxParser_.feed(buffer, size)) {
    LOG_DBG("COF", "Parse error at line %d: %s", saxParser_.errorLine(), saxParser_.errorString());
    return 0;
  }
  if (remainingSize == 0) {
    if (!saxParser_.finalize()) {
      LOG_DBG("COF", "Parse error (finalize): %s", saxParser_.errorString());
      return 0;
    }
  }
  return size;
}

void ContentOpfParser::beginSpineTable() {
  spinePending_ = 0;
  if (spineTable_ != nullptr) return;  // lent
  PassTable<SpineTarget>::HeapSizing sizing;
  // At most one table-full of slots a SpineTarget can number.
  const size_t wanted = std::min<size_t>(spineCandidates_ > 0 ? spineCandidates_ : 1, UINT16_MAX);
  ownSpineTable_ = PassTable<SpineTarget>::fromHeap(wanted, sizing);
  if (ownSpineTable_.capacity() > 0) {
    spineTable_ = ownSpineTable_.entries();
    spineCapacity_ = ownSpineTable_.capacity();
  } else {
    // No heap for a table: one itemref per pass, the resumed scan of old.
    spineTable_ = &inlineSpineTarget_;
    spineCapacity_ = 1;
  }
}

void ContentOpfParser::queueSpineItemRef(const char* idref) {
  if (spineTable_ == nullptr || !itemReader_.has_value()) return;  // no item store: nothing to resolve against
  const size_t len = strlen(idref);
  SpineTarget& t = spineTable_[spinePending_];
  t.idHash = HashUtils::fnvHash64(idref, len);
  t.idLen = static_cast<uint16_t>(len);
  t.slot = static_cast<uint16_t>(spinePending_);
  t.hrefOffset = NO_HREF;
  if (++spinePending_ == spineCapacity_) resolvePendingSpineItemRefs();
}

void ContentOpfParser::resolvePendingSpineItemRefs() {
  const size_t count = spinePending_;
  spinePending_ = 0;
  if (count == 0 || !itemReader_.has_value()) return;
  spinePasses_++;
  const auto byKey = [](const SpineTarget& a, const SpineTarget& b) {
    return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
  };
  std::sort(spineTable_, spineTable_ + count, byKey);

  // One pass from the resume point to the end and on from the start up to it, ending early once
  // every itemref of the table-full has its href. An itemref found when it is the next one due in
  // spine order goes to the cache at once, its href still in the read window: a spine in manifest
  // order is written out by the pass itself and the store read about once. The others are read
  // back by offset afterwards, a small read each.
  const auto emit = [this](const uint32_t hrefOffset) {
    if (itemReader_->readStringAt(hrefOffset, hrefScratch_) && cache) cache->createSpineEntry(hrefScratch_);
  };
  const uint32_t storeSize = static_cast<uint32_t>(tempItemStore.fileSize());
  const uint32_t resumeOffset = itemScanResumeOffset_ < storeSize ? itemScanResumeOffset_ : 0;
  size_t matched = 0;
  size_t nextSlot = 0;
  uint32_t lastMatchEnd = resumeOffset;
  for (int pass = 0; pass < 2 && matched < count; pass++) {
    const uint32_t end = pass == 0 ? storeSize : resumeOffset;
    itemReader_->seek(pass == 0 ? resumeOffset : 0);
    while (itemReader_->position() < end && matched < count) {
      const uint32_t recordStart = itemReader_->position();
      uint32_t hrefLen = 0;
      if (!itemReader_->readString(itemIdScratch_)) break;
      const uint32_t hrefOffset = itemReader_->position();
      if (!itemReader_->readPod(hrefLen)) break;
      const uint32_t recordEnd = itemReader_->position() + hrefLen;
      // Guard against malformed temp data or host shims that don't signal EOF via available().
      if (recordEnd <= recordStart) break;
      const SpineTarget key = {HashUtils::fnvHash64(itemIdScratch_), static_cast<uint16_t>(itemIdScratch_.size()), 0,
                               0};
      for (SpineTarget* it = std::lower_bound(spineTable_, spineTable_ + count, key, byKey);
           it != spineTable_ + count && it->idHash == key.idHash && it->idLen == key.idLen; ++it) {
        if (it->hrefOffset != NO_HREF) continue;  // an id the manifest repeats: its first item wins
        matched++;
        lastMatchEnd = recordEnd;
        if (it->slot == nextSlot) {
          emit(hrefOffset);
          it->hrefOffset = EMITTED;
          nextSlot++;
        } else {
          it->hrefOffset = hrefOffset;
        }
      }
      itemReader_->seek(recordEnd);
    }
  }
  itemScanResumeOffset_ = lastMatchEnd < storeSize ? lastMatchEnd : 0;

  // The rest, in spine order: every slot before nextSlot is out already.
  std::sort(spineTable_, spineTable_ + count,
            [](const SpineTarget& a, const SpineTarget& b) { return a.slot < b.slot; });
  for (size_t i = nextSlot; i < count; i++) {
    const SpineTarget& t = spineTable_[i];
    if (t.hrefOffset == NO_HREF || t.hrefOffset == EMITTED) continue;  // NO_HREF: not in the manifest
    emit(t.hrefOffset);
  }
}

void ContentOpfParser::handleSpineItemRefElement(const char** atts) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], "idref") == 0) {
      stats.itemRefCount++;
      queueSpineItemRef(atts[i + 1]);
    }
  }
}

void ContentOpfParser::startElement(void* userData, const char* name, const char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)atts;

  if (self->state == START && isPackageTag(name)) {
    self->state = IN_PACKAGE;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "unique-identifier") == 0) self->uniqueIdentifierId_ = atts[i + 1];
    }
    return;
  }

  if (self->state == IN_PACKAGE && isMetadataTag(name)) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:title") == 0) {
    // Only capture the first dc:title element; subsequent ones are subtitles
    if (self->title.empty()) {
      self->state = IN_BOOK_TITLE;
    }
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:creator") == 0) {
    self->state = IN_BOOK_AUTHOR;
    self->creatorText_.clear();
    self->currentCreator_ = self->beginCreator(atts);
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:language") == 0) {
    self->state = IN_BOOK_LANGUAGE;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:identifier") == 0) {
    self->state = IN_BOOK_IDENTIFIER;
    self->identifierText_.clear();
    self->identifierTooLong_ = false;
    self->identifierScheme_ = IdentifierScheme::Other;
    self->identifierIsUnique_ = false;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        self->identifierIsUnique_ = !self->uniqueIdentifierId_.empty() && self->uniqueIdentifierId_ == atts[i + 1];
      }
      if (strcmp(atts[i], "opf:scheme") != 0 && strcmp(atts[i], "scheme") != 0) continue;
      std::string scheme = atts[i + 1];
      for (char& c : scheme) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      if (scheme.find("isbn") != std::string::npos) {
        self->identifierScheme_ = IdentifierScheme::Isbn;
      } else if (scheme.find("asin") != std::string::npos || scheme == "amazon") {
        self->identifierScheme_ = IdentifierScheme::Asin;
      }
    }
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:description") == 0) {
    // Only capture the first dc:description element; subsequent ones are alternate/localized variants
    if (self->description.empty()) {
      self->state = IN_BOOK_DESCRIPTION;
    }
    return;
  }

  if (self->state == IN_PACKAGE && isManifestTag(name)) {
    self->state = IN_MANIFEST;
    // The item store exists for ONE job: resolving spine idrefs to hrefs, which only happens when
    // there is a cache to write spine entries into. A cover/metadata load (OpfCacheMode::Disabled)
    // passes a null cache and skips spine parsing entirely, so building the store there wrote the
    // whole manifest to SD for nothing — and, because those load paths never call setupCacheDir(),
    // usually failed to open at all and logged three "probably going to be a fatal error" lines per
    // book per pass. coverItemHref does not come from here; it is matched inline as items stream by.
    if (self->cache) {
      if (!Storage.openFileForWrite("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
        LOG_ERR("COF", "Couldn't open temp items file for writing. This is probably going to be a fatal error.");
      }
      self->itemWriter_.emplace(self->tempItemStore);
    }
    return;
  }

  if (self->state == IN_PACKAGE && isSpineTag(name)) {
    self->state = IN_SPINE;
    // No cache => no spine parsing (see the isItemRefTag branch), so nothing will read this.
    // queueSpineItemRef() already treats an absent reader as "cannot resolve".
    if (self->cache) {
      if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
        LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
      }
      self->itemReader_.emplace(self->tempItemStore);
      self->beginSpineTable();
    }
    return;
  }

  if (self->state == IN_PACKAGE && isGuideTag(name)) {
    self->state = IN_GUIDE;
    // TODO Remove print
    LOG_DBG("COF", "Entering guide state.");
    // Guide references carry their own href attribute and never consult the item store, so this
    // open only has to keep the (cached) reader pointing at a valid handle.
    if (self->cache) {
      if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
        LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
      }
    }
    return;
  }

  if (self->state == IN_METADATA && isMetaTag(name)) {
    const char* metaName = nullptr;
    const char* metaContent = nullptr;
    const char* metaProperty = nullptr;
    const char* metaRefines = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0) {
        metaName = atts[i + 1];
      } else if (strcmp(atts[i], "content") == 0) {
        metaContent = atts[i + 1];
      } else if (strcmp(atts[i], "property") == 0) {
        metaProperty = atts[i + 1];
      } else if (strcmp(atts[i], "refines") == 0) {
        metaRefines = atts[i + 1];
      }
    }

    // EPUB 3 creator refinements: <meta refines="#c1" property="role">aut</meta> and
    // property="file-as". Only for an id that names a creator; a refinement of anything else (a
    // collection's group-position, a title's file-as) falls through to the handling below.
    if (metaProperty && metaRefines && metaRefines[0] == '#') {
      if (std::string* field = self->creatorField(metaRefines + 1, metaProperty)) {
        field->clear();
        if (metaContent) {
          appendCapped(*field, metaContent, strlen(metaContent), MAX_CREATOR_FIELD);
        } else {
          self->refineTarget_ = field;
          self->state = IN_CREATOR_REFINE;
        }
        return;
      }
    }

    if (metaName && metaContent) {
      if (strcmp(metaName, "cover") == 0) {
        self->coverItemId = metaContent;
      } else if (strcmp(metaName, "calibre:series") == 0 && self->series.empty()) {
        self->series = trim(std::string(metaContent, std::min(strlen(metaContent), size_t{MAX_DESCRIPTION_LENGTH})));
      } else if (strcmp(metaName, "calibre:series_index") == 0 && self->seriesIndex.empty()) {
        self->seriesIndex =
            trim(std::string(metaContent, std::min(strlen(metaContent), size_t{MAX_DESCRIPTION_LENGTH})));
      }
    }

    // EPUB 3 collection metadata:
    // <meta property="belongs-to-collection">Series Name</meta>  (character data)
    // <meta property="belongs-to-collection" content="Series Name"/>  (attribute, some generators)
    // <meta property="group-position">1</meta>
    if (metaProperty) {
      if (strcmp(metaProperty, "belongs-to-collection") == 0 && self->series.empty()) {
        if (metaContent) {
          self->series = trim(std::string(metaContent, std::min(strlen(metaContent), size_t{MAX_DESCRIPTION_LENGTH})));
        } else {
          self->state = IN_BOOK_SERIES;
          return;
        }
      }
      if (strcmp(metaProperty, "group-position") == 0 && self->seriesIndex.empty()) {
        if (metaContent) {
          self->seriesIndex =
              trim(std::string(metaContent, std::min(strlen(metaContent), size_t{MAX_DESCRIPTION_LENGTH})));
        } else {
          self->state = IN_BOOK_SERIES_INDEX;
          return;
        }
      }
    }

    return;
  }

  if (self->state == IN_MANIFEST && isItemTag(name)) {
    std::string itemId;
    std::string href;
    // media-type/properties are only INSPECTED, never stored: keep them as pointers into the
    // parser's attribute array (valid for this callback) instead of copying to std::string.
    // The dominant media type, application/xhtml+xml, is 21 chars — past SSO — so the old copy
    // heap-allocated once per item (~1500 alloc/free pairs on a large Calibre manifest).
    const char* mediaType = nullptr;
    const char* properties = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        itemId = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      } else if (strcmp(atts[i], "media-type") == 0) {
        mediaType = atts[i + 1];
      } else if (strcmp(atts[i], "properties") == 0) {
        properties = atts[i + 1];
      }
    }

    // Manifests group items of one media type together (all chapters, then all images, ...),
    // so the previous item's classification almost always applies: one strcmp against the memo
    // replaces the per-item prefix/equality chain. A distinct value is classified once and
    // cached in the fixed member buffer (no heap; values longer than the buffer classify
    // per-item, which no real media type triggers).
    MediaClass mediaClass = MediaClass::Other;
    if (mediaType != nullptr) {
      if (strcmp(mediaType, self->lastMediaType_) == 0) {
        mediaClass = static_cast<MediaClass>(self->lastMediaClass_);
      } else {
        mediaClass = classifyMediaType(mediaType);
        if (strlen(mediaType) < sizeof(self->lastMediaType_)) {
          strcpy(self->lastMediaType_, mediaType);
          self->lastMediaClass_ = static_cast<uint8_t>(mediaClass);
        }
      }
    }

    // Write items down to SD card (buffered — thousands of tiny field writes otherwise cost a
    // full FsFile call each). The spine passes read them back (resolvePendingSpineItemRefs).
    if (self->itemWriter_.has_value()) self->itemWriter_->writeString(itemId);
    if (mediaClass == MediaClass::Other) self->spineCandidates_++;
    if (self->itemWriter_.has_value()) self->itemWriter_->writeString(href);

    if (itemId == self->coverItemId) {
      // Some EPUBs set meta name="cover" to an XHTML wrapper item.
      // Only treat it as a cover image when the manifest media-type is image/*.
      if (mediaClass == MediaClass::Image) {
        self->coverItemHref = href;
      } else {
        LOG_DBG("COF", "Ignoring meta cover item '%s' with non-image media type: %s", itemId.c_str(),
                mediaType != nullptr ? mediaType : "(none)");
      }
    }

    switch (mediaClass) {
      case MediaClass::Ncx:
        if (self->tocNcxPath.empty()) {
          self->tocNcxPath = href;
        } else {
          LOG_DBG("COF", "Warning: Multiple NCX files found in manifest. Ignoring duplicate: %s", href.c_str());
        }
        break;
      // EPUB 2.01 page-map.xml — separate top-level file mapping printed page numbers to spine
      // locations (e.g. <page name="1" href="OEBPS/c9_split_000.xhtml"/>). Spine references it
      // via <spine page-map="..."> but only the manifest item carries the canonical href.
      case MediaClass::PageMap:
        if (self->pageMapPath.empty()) {
          self->pageMapPath = href;
          LOG_DBG("COF", "Found EPUB 2.01 page-map: %s", href.c_str());
        }
        break;
      case MediaClass::Css:
        self->cssFiles.push_back(href);
        break;
      case MediaClass::Image:
      case MediaClass::Other:
        break;
    }

    // EPUB 3: Check for nav document (properties contains "nav" as a whole token)
    if (self->tocNavPath.empty() && hasPropertyWord(properties, "nav")) {
      self->tocNavPath = href;
      LOG_DBG("COF", "Found EPUB 3 nav document: %s", href.c_str());
    }

    // EPUB 3: Check for cover image (properties contains "cover-image" as a whole token)
    if (self->coverItemHref.empty() && hasPropertyWord(properties, "cover-image")) {
      self->coverItemHref = href;
    }
    return;
  }

  // NOTE: This relies on spine appearing after item manifest (which is pretty safe as it's part of the EPUB spec)
  // Only run the spine parsing if there's a cache to add it to
  if (self->cache) {
    if (self->state == IN_SPINE && isItemRefTag(name)) {
      self->handleSpineItemRefElement(atts);
      return;
    }
  }
  // parse the guide
  if (self->state == IN_GUIDE && isReferenceTag(name)) {
    std::string type;
    std::string guideHref;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        type = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        guideHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      }
    }
    if (!guideHref.empty()) {
      if (type == "text" || (type == "start" && !self->textReferenceHref.empty())) {
        LOG_DBG("COF", "Found %s reference in guide: %s", type.c_str(), guideHref.c_str());
        self->textReferenceHref = guideHref;
      } else if ((type == "cover" || type == "cover-page") && self->guideCoverPageHref.empty()) {
        LOG_DBG("COF", "Found cover reference in guide: %s", guideHref.c_str());
        self->guideCoverPageHref = guideHref;
      }
    }
    return;
  }
}

void ContentOpfParser::characterData(void* userData, const char* s, const int len) {
  auto* self = static_cast<ContentOpfParser*>(userData);

  if (self->state == IN_BOOK_TITLE) {
    self->title.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_AUTHOR) {
    appendCapped(self->creatorText_, s, static_cast<size_t>(len), MAX_CREATOR_FIELD);
    return;
  }

  if (self->state == IN_CREATOR_REFINE) {
    if (self->refineTarget_) appendCapped(*self->refineTarget_, s, static_cast<size_t>(len), MAX_CREATOR_FIELD);
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE) {
    self->language.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_IDENTIFIER) {
    if (self->identifierText_.size() + static_cast<size_t>(len) > MAX_IDENTIFIER) {
      self->identifierTooLong_ = true;
    } else if (!self->identifierTooLong_) {
      self->identifierText_.append(s, len);
    }
    return;
  }

  if (self->state == IN_BOOK_DESCRIPTION) {
    if (self->description.size() < MAX_DESCRIPTION_LENGTH) {
      const size_t remaining = MAX_DESCRIPTION_LENGTH - self->description.size();
      self->description.append(s, std::min(static_cast<size_t>(len), remaining));
    }
    return;
  }

  if (self->state == IN_BOOK_SERIES) {
    if (self->series.size() < MAX_DESCRIPTION_LENGTH) {
      const size_t remaining = MAX_DESCRIPTION_LENGTH - self->series.size();
      self->series.append(s, std::min(static_cast<size_t>(len), remaining));
    }
    return;
  }

  if (self->state == IN_BOOK_SERIES_INDEX) {
    if (self->seriesIndex.size() < MAX_DESCRIPTION_LENGTH) {
      const size_t remaining = MAX_DESCRIPTION_LENGTH - self->seriesIndex.size();
      self->seriesIndex.append(s, std::min(static_cast<size_t>(len), remaining));
    }
    return;
  }
}

void ContentOpfParser::endElement(void* userData, const char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)name;

  if (self->state == IN_SPINE && isSpineTag(name)) {
    self->state = IN_PACKAGE;
    self->resolvePendingSpineItemRefs();
    if (self->spineTable_ != nullptr) {
      LOG_DBG("COF", "Spine: %u itemrefs in %u passes over the item store, %u a pass (%s)",
              static_cast<unsigned>(self->stats.itemRefCount), static_cast<unsigned>(self->spinePasses_),
              static_cast<unsigned>(self->spineCapacity_), self->spineTableLent_ ? "lent region" : "heap");
    }
    self->ownSpineTable_ = PassTable<SpineTarget>();
    self->spineTable_ = nullptr;
    self->spineCapacity_ = 0;
    self->itemReader_.reset();
    self->closeItemStore();
    return;
  }

  if (self->state == IN_GUIDE && isGuideTag(name)) {
    self->state = IN_PACKAGE;
    self->closeItemStore();
    return;
  }

  if (self->state == IN_MANIFEST && isManifestTag(name)) {
    self->state = IN_PACKAGE;
    // Flush buffered item records before the file closes; the spine pass reads them back.
    if (self->itemWriter_.has_value()) {
      self->itemWriter_->flush();
      self->itemWriter_.reset();
    }
    self->closeItemStore();
    return;
  }

  if (self->state == IN_BOOK_TITLE && strcmp(name, "dc:title") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && strcmp(name, "dc:creator") == 0) {
    // Joined here, once per creator, so a name that arrived in pieces is still one name.
    std::string text = trim(self->creatorText_);
    self->creatorText_.clear();
    if (!text.empty()) {
      if (!self->author.empty()) self->author.append(", ");
      self->author.append(text);
    }
    if (self->currentCreator_) self->currentCreator_->name = std::move(text);
    self->currentCreator_ = nullptr;
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_CREATOR_REFINE && isMetaTag(name)) {
    if (self->refineTarget_) *self->refineTarget_ = trim(*self->refineTarget_);
    self->refineTarget_ = nullptr;
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && strcmp(name, "dc:language") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_IDENTIFIER && strcmp(name, "dc:identifier") == 0) {
    self->recordIdentifier();
    self->identifierText_.clear();
    self->identifierText_.shrink_to_fit();
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_DESCRIPTION && strcmp(name, "dc:description") == 0) {
    self->description = stripHtml(self->description);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_SERIES && isMetaTag(name)) {
    self->series = trim(self->series);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_SERIES_INDEX && isMetaTag(name)) {
    self->seriesIndex = trim(self->seriesIndex);
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && isMetadataTag(name)) {
    self->pickPrimaryAuthor();
    self->state = IN_PACKAGE;
    self->metadataRead_ = true;
    return;
  }

  if (self->state == IN_PACKAGE && isPackageTag(name)) {
    self->state = START;
    return;
  }
}
