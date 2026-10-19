#include "TtfFamily.h"

#include <algorithm>
#include <cstring>

namespace ttf {
namespace {

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
         p[3];
}
constexpr uint32_t tag(const char (&t)[5]) {
  return static_cast<uint32_t>(static_cast<uint8_t>(t[0])) << 24 |
         static_cast<uint32_t>(static_cast<uint8_t>(t[1])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(t[2])) << 8 | static_cast<uint8_t>(t[3]);
}

// The tables parseFace() needs; a file without one of them can never draw a glyph.
constexpr uint32_t kRequired[] = {tag("cmap"), tag("glyf"), tag("head"), tag("hhea"),
                                  tag("hmtx"), tag("loca"), tag("maxp")};
constexpr uint32_t kAllRequired = (1u << (sizeof kRequired / sizeof kRequired[0])) - 1;
// What an SD-mode block lists: every table the engine reads (parseFace(), the layout readers), all copied but glyf.
// A font cannot draw without the first seven; the others it draws without, as parseFace() takes them absent when
// their record lies outside the file.
constexpr uint32_t kSdTables[] = {tag("cmap"), tag("head"), tag("hhea"), tag("hmtx"), tag("loca"), tag("maxp"),
                                  tag("glyf"), tag("OS/2"), tag("GPOS"), tag("GSUB"), tag("kern")};
constexpr int kSdGlyf = 6;
constexpr int kSdFirstOptional = 7;

// Which of kSdTables a record is, or -1, and whether it is its tag's first record: parseFace() reads only that one.
// `seen` remembers the tags met.
int sdTable(const uint32_t tableTag, uint16_t& seen) {
  for (int k = 0; k < static_cast<int>(sizeof kSdTables / sizeof kSdTables[0]); ++k) {
    if (tableTag != kSdTables[k]) continue;
    const bool first = (seen >> k & 1u) == 0;
    seen |= static_cast<uint16_t>(1u << k);
    return first ? k : -1;
  }
  return -1;
}
// An SD-mode block's bytes: the sfnt header, a record per copied table and one for glyf, then the tables.
constexpr uint64_t sdBlockBytes(const uint32_t copied, const uint64_t tableBytes) {
  return 12 + 16 * (copied + 1) + tableBytes;
}
constexpr uint16_t kMaxDirectory = 256;  // real fonts have under 40 tables; this bounds a hostile directory's reads
constexpr uint32_t kHeadBytes = 54;
constexpr uint32_t kHeadMagic = 0x5F0F3CF5;
constexpr uint32_t kMacStyleOffset = 44;
constexpr uint32_t kWeightClassOffset = 4;  // OS/2 usWeightClass

void putLe(uint8_t* out, const uint32_t value, const int bytes) {
  for (int i = 0; i < bytes; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

// A table directory record as the file has it.
struct Record {
  uint32_t tag;
  uint32_t checksum;
  uint32_t offset;
  uint32_t length;
};

// Calls `visit(ctx, record)` for each record of `file`'s table directory, read eight records at a time. False when
// the file is not TrueType (CFF 'OTTO', a collection 'ttcf'), its directory does not fit it, a read fails, or `visit`
// returns false. `version` gets the sfnt version. A function, not a template: the registry's probe and SD mode's
// block share one copy.
bool forEachTable(const FileSource& file, uint32_t* version, bool (*visit)(void* ctx, const Record& record),
                  void* ctx) {
  uint8_t header[12];
  if (!file.read || file.size < sizeof header || !file.read(file.ctx, 0, header, sizeof header)) return false;
  *version = be32(header);
  if (*version != 0x00010000 && *version != tag("true")) return false;
  const uint16_t numTables = be16(header + 4);
  if (numTables == 0 || numTables > kMaxDirectory || sizeof header + 16u * numTables > file.size) return false;
  uint8_t records[8 * 16];
  for (uint16_t first = 0; first < numTables; first += 8) {
    const uint16_t n = std::min<uint16_t>(8, static_cast<uint16_t>(numTables - first));
    if (!file.read(file.ctx, sizeof header + 16u * first, records, 16u * n)) return false;
    for (uint16_t i = 0; i < n; ++i) {
      const uint8_t* raw = records + 16 * i;
      if (!visit(ctx, {be32(raw), be32(raw + 4), be32(raw + 8), be32(raw + 12)})) return false;
    }
  }
  return true;
}

bool inFile(const FileSource& file, const uint32_t offset, const uint32_t length) {
  return offset <= file.size && length <= file.size - offset;
}

}  // namespace

bool isTtfFileName(const char* name) {
  const size_t len = std::strlen(name);
  if (len <= 4 || name[len - 4] != '.') return false;
  const char* ext = name + len - 3;
  return (ext[0] | 0x20) == 't' && (ext[1] | 0x20) == 't' && (ext[2] | 0x20) == 'f';
}

bool assignStyles(const FileProbe* files, const size_t count, int (&out)[4]) {
  // Lower is better: a variable font ranks after every static one, then the distance from the style's weight, then
  // a file declaring the style before one that only weighs as it (below).
  const auto rank = [&](const size_t i, const int style) {
    const int target = (style & 1) ? 700 : 400;
    const int distance = files[i].weight > target ? files[i].weight - target : target - files[i].weight;
    return (files[i].variable ? 0x10000 : 0) + 2 * distance + (files[i].style != style ? 1 : 0);
  };
  const auto offer = [&](const size_t i, const int style) {
    if (out[style] < 0 || rank(i, style) < rank(static_cast<size_t>(out[style]), style)) {
      out[style] = static_cast<int>(i);
    }
  };
  for (int& file : out) file = -1;
  for (size_t i = 0; i < count; ++i) {
    if (files[i].style >= 0 && files[i].style <= 3) offer(i, files[i].style);
  }
  // A bold weight whose macStyle says regular, as some exports mark a Bold only by its OS/2 weight class (Inter's
  // static Inter-Bold.ttf, embedded book fonts made from it), may serve the bold style too: not when it already
  // serves the regular one, since a single heavy face is a regular with no bold.
  for (size_t i = 0; i < count; ++i) {
    const int style = files[i].style;
    if ((style == 0 || style == 2) && files[i].weight >= 600 && out[style] != static_cast<int>(i)) offer(i, style | 1);
  }
  // No regular face (a script that is only italic, a display face only bold, as books embed them): the nearest style
  // serves as the regular, and the slot it leaves falls back to it. Moved, not shared, so the store holds it once.
  for (const int style : {2, 1, 3}) {
    if (out[0] >= 0) break;
    out[0] = out[style];
    out[style] = -1;
  }
  return out[0] >= 0;
}

int probeStyle(const FileSource& file) { return probeFile(file).style; }

bool fitsStore(const uint32_t* fileBytes, const uint32_t* sdBytes, const uint8_t count, const uint32_t usableBytes,
               const uint32_t freeMapPages) {
  if (count == 0 || count > glyphstore::kMaxStyles) return false;
  glyphstore::PlanInput in{};
  in.styleCount = count;
  in.usableBytes = usableBytes;
  in.freeMapPages = freeMapPages;
  glyphstore::Layout layout{};
  for (const uint8_t mode : {glyphstore::kModeFlash, glyphstore::kModeSd}) {
    for (uint8_t s = 0; s < count; ++s) {
      in.styleMode[s] = mode;
      in.styleBytes[s] = mode == glyphstore::kModeFlash ? fileBytes[s] : sdBytes[s];
    }
    if (glyphstore::planLayout(in, layout)) return true;
  }
  return false;
}

FileProbe probeFile(const FileSource& file) {
  struct Scan {
    const FileSource& file;
    FileProbe probe;
    uint32_t found = 0;
    uint32_t headOffset = 0;
    uint32_t os2Offset = 0;
    bool hasOs2Weight = false;
    uint16_t sdSeen = 0;
    uint32_t sdTables = 0;
    uint64_t sdTableBytes = 0;
  } scan{file};
  uint32_t version = 0;
  const auto visit = [](void* ctx, const Record& record) {
    Scan& scan = *static_cast<Scan*>(ctx);
    if (record.tag == tag("fvar")) scan.probe.variable = true;
    const int sd = sdTable(record.tag, scan.sdSeen);
    if (sd >= 0 && sd != kSdGlyf && inFile(scan.file, record.offset, record.length)) {
      ++scan.sdTables;
      scan.sdTableBytes += glyphstore::align4(record.length);
    }
    if (record.tag == tag("OS/2") && inFile(scan.file, record.offset, record.length) && record.length >= 6) {
      scan.os2Offset = record.offset;
      scan.hasOs2Weight = true;
    }
    for (uint32_t k = 0; k < sizeof kRequired / sizeof kRequired[0]; ++k) {
      if (record.tag != kRequired[k]) continue;
      if (!inFile(scan.file, record.offset, record.length)) return false;
      if (record.tag == tag("head")) {
        if (record.length < kHeadBytes) return false;
        scan.headOffset = record.offset;
      }
      scan.found |= 1u << k;
    }
    return true;
  };
  if (!forEachTable(file, &version, visit, &scan) || scan.found != kAllRequired) return {};
  FileProbe& probe = scan.probe;
  const uint32_t headOffset = scan.headOffset;
  // As SdBlock::plan() lays it out, by the same rule (sdTable()).
  const uint64_t sdBytes = sdBlockBytes(scan.sdTables, scan.sdTableBytes);
  probe.sdBytes = sdBytes < UINT32_MAX ? static_cast<uint32_t>(sdBytes) : UINT32_MAX;

  uint8_t head[kHeadBytes];
  if (!file.read(file.ctx, headOffset, head, sizeof head) || be32(head + 12) != kHeadMagic) return {};
  probe.style = be16(head + kMacStyleOffset) & 3;
  probe.weight = (probe.style & 1) ? 700 : 400;
  uint8_t weight[2];
  if (scan.hasOs2Weight && file.read(file.ctx, scan.os2Offset + kWeightClassOffset, weight, sizeof weight) &&
      be16(weight) != 0) {
    probe.weight = be16(weight);
  }
  return probe;
}

// --- SD mode's style block ---

bool SdBlock::plan(const FileSource& file) {
  file_ = file;
  seen_ = 0;
  count_ = 0;
  bytes_ = 0;
  glyf_ = {};
  const auto visit = [](void* ctx, const Record& record) { return static_cast<SdBlock*>(ctx)->addTable(&record); };
  if (!forEachTable(file, &version_, visit, this)) return false;
  // The directory, then the tables, each 4-byte aligned; glyf at the end, past them all (sdBlockBytes()).
  uint64_t end = 12 + 16u * count_;
  int glyf = -1;
  for (uint8_t i = 0; i < count_; ++i) {
    if (tables_[i].tag == kSdTables[kSdGlyf]) {
      glyf = i;
      continue;
    }
    tables_[i].blockOffset = static_cast<uint32_t>(end);
    end += glyphstore::align4(tables_[i].length);
    if (end > UINT32_MAX - file.size) return false;  // the block and glyf past it stay in 32 bits, whatever the tables
  }
  if (glyf < 0) return false;
  bytes_ = static_cast<uint32_t>(end);
  tables_[glyf].blockOffset = bytes_;
  glyf_ = tables_[glyf];
  return true;
}

// A directory record (a Record): kept when SD mode copies its table, or it is glyf.
bool SdBlock::addTable(const void* record) {
  const Record& table = *static_cast<const Record*>(record);
  const int k = sdTable(table.tag, seen_);
  if (k < 0) return true;
  // A table the font needs outside the file: it cannot be drawn. An optional one: absent, as parseFace() takes it.
  if (!inFile(file_, table.offset, table.length)) return k >= kSdFirstOptional;
  // In file order, so the install reads the file forward.
  uint8_t at = count_++;
  for (; at > 0 && tables_[at - 1].fileOffset > table.offset; --at) tables_[at] = tables_[at - 1];
  tables_[at] = {table.tag, table.checksum, table.offset, table.length, 0};
  return true;
}

// Word `index` of the block's table directory: the sfnt header (nothing reads its search fields, left 0), then a
// record per table.
uint32_t SdBlock::directoryWord(const uint32_t index) const {
  if (index == 0) return version_;
  if (index == 1) return static_cast<uint32_t>(count_) << 16;
  if (index == 2) return 0;
  const Table& table = tables_[(index - 3) / 4];
  switch ((index - 3) % 4) {
    case 0:
      return table.tag;
    case 1:
      return table.checksum;
    case 2:
      return table.blockOffset;
    default:
      return table.length;
  }
}

bool SdBlock::read(uint32_t offset, uint8_t* dst, uint32_t len) const {
  if (offset > bytes_ || len > bytes_ - offset) return false;
  const uint32_t directory = 12 + 16u * count_;
  while (len > 0) {
    uint32_t n = 1;
    if (offset < directory) {
      *dst = static_cast<uint8_t>(directoryWord(offset / 4) >> (24 - 8 * (offset % 4)));
    } else {
      // The table this byte is in (glyf, past the block, never is), else the padding before the next one.
      const Table* in = nullptr;
      uint32_t next = bytes_;
      for (uint8_t i = 0; i < count_; ++i) {
        const Table& table = tables_[i];
        if (offset >= table.blockOffset && offset - table.blockOffset < table.length) in = &table;
        if (table.blockOffset > offset && table.blockOffset < next) next = table.blockOffset;
      }
      if (in) {
        n = std::min(len, in->blockOffset + in->length - offset);
        if (!file_.read(file_.ctx, in->fileOffset + (offset - in->blockOffset), dst, n)) return false;
      } else {
        n = std::min(len, next - offset);
        std::memset(dst, 0, n);
      }
    }
    dst += n;
    offset += n;
    len -= n;
  }
  return true;
}

bool SdBlock::sameAs(const uint8_t* block, const uint32_t size) const {
  if (block == nullptr || size != bytes_) return false;
  for (uint32_t word = 0; word < 3 + 4u * count_; ++word) {
    if (directoryWord(word) != be32(block + 4 * word)) return false;
  }
  return true;
}

bool SdBlock::readSource(void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
  return static_cast<const SdBlock*>(ctx)->read(offset, static_cast<uint8_t*>(dst), len);
}

void familyIdentity(const FileStamp (&styles)[4], uint8_t out[glyphstore::kIdentityBytes]) {
  for (uint32_t lane = 0; lane < glyphstore::kIdentityBytes / 4; ++lane) {
    uint32_t hash = 2166136261u ^ (lane * 0x9E3779B9u);
    for (uint8_t style = 0; style < 4; ++style) {
      const FileStamp& file = styles[style];
      const uint8_t marker = file.name ? style : static_cast<uint8_t>(0x80 | style);
      hash = glyphstore::fnv1a(&marker, 1, hash);
      if (!file.name) continue;
      hash = glyphstore::fnv1a(file.name, std::strlen(file.name) + 1, hash);
      uint8_t fields[8];
      putLe(fields, file.size, 4);
      putLe(fields + 4, file.date, 2);
      putLe(fields + 6, file.time, 2);
      hash = glyphstore::fnv1a(fields, sizeof fields, hash);
    }
    hash = glyphstore::fnv1a(&kRenderVersion, 1, hash);
    putLe(out + 4 * lane, hash, 4);
  }
}

void setIdentity(const uint8_t (*families)[glyphstore::kIdentityBytes], const uint8_t count,
                 uint8_t out[glyphstore::kIdentityBytes]) {
  for (uint32_t lane = 0; lane < glyphstore::kIdentityBytes / 4; ++lane) {
    uint32_t hash = glyphstore::fnv1a(&count, 1, 2166136261u ^ (lane * 0x9E3779B9u));
    for (uint8_t family = 0; family < count; ++family) {
      hash = glyphstore::fnv1a(families[family], glyphstore::kIdentityBytes, hash);
    }
    putLe(out + 4 * lane, hash, 4);
  }
}

}  // namespace ttf
