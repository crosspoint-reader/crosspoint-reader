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
constexpr uint16_t kMaxTables = 256;  // real fonts have under 40; this bounds a hostile directory's reads
constexpr uint32_t kHeadBytes = 54;
constexpr uint32_t kHeadMagic = 0x5F0F3CF5;
constexpr uint32_t kMacStyleOffset = 44;
constexpr uint32_t kWeightClassOffset = 4;  // OS/2 usWeightClass

void putLe(uint8_t* out, const uint32_t value, const int bytes) {
  for (int i = 0; i < bytes; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
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
  return out[0] >= 0;
}

int probeStyle(const FileSource& file) { return probeFile(file).style; }

bool fitsFlashMode(const uint32_t* styleBytes, const uint8_t count, const uint32_t usableBytes,
                   const uint32_t freeMapPages) {
  if (count == 0 || count > glyphstore::kMaxStyles) return false;
  glyphstore::PlanInput in{};
  in.mode = glyphstore::kModeFlash;
  in.styleCount = count;
  for (uint8_t s = 0; s < count; ++s) in.styleBytes[s] = styleBytes[s];
  in.usableBytes = usableBytes;
  in.freeMapPages = freeMapPages;
  glyphstore::Layout layout{};
  return glyphstore::planLayout(in, layout);
}

FileProbe probeFile(const FileSource& file) {
  FileProbe probe;
  uint8_t header[12];
  if (!file.read || file.size < sizeof header || !file.read(file.ctx, 0, header, sizeof header)) return probe;
  const uint32_t version = be32(header);
  if (version != 0x00010000 && version != tag("true")) return probe;  // CFF ('OTTO') and collections ('ttcf')
  const uint16_t numTables = be16(header + 4);
  if (numTables == 0 || numTables > kMaxTables || sizeof header + 16u * numTables > file.size) return probe;

  uint32_t found = 0;
  uint32_t headOffset = 0;
  uint32_t os2Offset = 0;
  bool hasOs2Weight = false;
  uint8_t records[8 * 16];
  for (uint16_t first = 0; first < numTables; first += 8) {
    const uint16_t n = std::min<uint16_t>(8, static_cast<uint16_t>(numTables - first));
    if (!file.read(file.ctx, sizeof header + 16u * first, records, 16u * n)) return probe;
    for (uint16_t i = 0; i < n; ++i) {
      const uint8_t* record = records + 16 * i;
      const uint32_t tableTag = be32(record);
      const uint32_t offset = be32(record + 8);
      const uint32_t length = be32(record + 12);
      if (tableTag == tag("fvar")) probe.variable = true;
      if (tableTag == tag("OS/2") && offset <= file.size && length <= file.size - offset && length >= 6) {
        os2Offset = offset;
        hasOs2Weight = true;
      }
      for (uint32_t k = 0; k < sizeof kRequired / sizeof kRequired[0]; ++k) {
        if (tableTag != kRequired[k]) continue;
        if (offset > file.size || length > file.size - offset) return {};
        if (tableTag == tag("head")) {
          if (length < kHeadBytes) return {};
          headOffset = offset;
        }
        found |= 1u << k;
      }
    }
  }
  if (found != kAllRequired) return {};

  uint8_t head[kHeadBytes];
  if (!file.read(file.ctx, headOffset, head, sizeof head) || be32(head + 12) != kHeadMagic) return {};
  probe.style = be16(head + kMacStyleOffset) & 3;
  probe.weight = (probe.style & 1) ? 700 : 400;
  uint8_t weight[2];
  if (hasOs2Weight && file.read(file.ctx, os2Offset + kWeightClassOffset, weight, sizeof weight) && be16(weight) != 0) {
    probe.weight = be16(weight);
  }
  return probe;
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

}  // namespace ttf
