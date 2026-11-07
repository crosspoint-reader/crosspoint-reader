#pragma once
#include <cstdint>
#include <cstring>

struct AdAskInfo {
  char filename[256]{};
  uint32_t fileCount = 0;
  uint64_t totalBytes = 0;
  bool sizesKnown = true;
};

// Read only the Files dictionary entries; never recurse into untrusted objects.
// Offsets and object references stay bounded by the binary plist trailer.
class AdAskParser {
 public:
  bool parse(const uint8_t* bytes, uint32_t size, AdAskInfo& out) {
    out = AdAskInfo{};
    data = bytes;
    length = size;
    if (size < 40 || memcmp(bytes, "bplist00", 8)) return false;
    const auto trailer = size - 32;
    offsetWidth = bytes[trailer + 6];
    refWidth = bytes[trailer + 7];
    uint64_t n, root, table;
    if (!integer(trailer + 8, 8, n) || !integer(trailer + 16, 8, root) || !integer(trailer + 24, 8, table) || !n ||
        n > 4096 || root >= n || table < 8 || table >= trailer || offsetWidth < 1 || offsetWidth > 8 || refWidth < 1 ||
        refWidth > 8 || n > (trailer - table) / offsetWidth)
      return false;
    objects = n;
    offsets = table;
    uint32_t files;
    if (!lookup(root, "Files", files)) return false;
    uint32_t pos, count;
    if (!container(files, 0xa, pos, count) || !count || count > 64 || count > (offsets - pos) / refWidth) return false;
    out.fileCount = count;
    for (uint32_t i = 0; i < count; ++i) {
      uint64_t dict;
      uint32_t name, value;
      if (!integer(pos + i * refWidth, refWidth, dict) || dict >= objects || !lookup(dict, "FileName", name))
        return false;
      // Validate each name, retaining the first for the confirmation popup.
      if (!string(name, i == 0 ? out.filename : nullptr, i == 0 ? sizeof(out.filename) : 0)) return false;
      if (!lookup(dict, "FileSize", value)) {
        out.sizesKnown = false;
        continue;
      }
      uint32_t off;
      uint64_t fileSize;
      if (!object(value, off) || (data[off] >> 4) != 1 || (data[off] & 15) > 3 ||
          (1u << (data[off] & 15)) > offsets - off - 1 || !integer(off + 1, 1u << (data[off] & 15), fileSize) ||
          fileSize > UINT64_MAX - out.totalBytes)
        return false;
      out.totalBytes += fileSize;
    }
    return true;
  }

 private:
  const uint8_t* data = nullptr;
  uint32_t length = 0, objects = 0, offsets = 0;
  uint8_t offsetWidth = 0, refWidth = 0;
  bool integer(uint32_t off, uint32_t width, uint64_t& result) const {
    if (width > 8 || off > length || width > length - off) return false;
    result = 0;
    for (uint32_t i = 0; i < width; ++i) result = (result << 8) | data[off + i];
    return true;
  }
  bool object(uint32_t id, uint32_t& off) const {
    uint64_t value;
    if (id >= objects || !integer(offsets + id * offsetWidth, offsetWidth, value) || value < 8 || value >= offsets)
      return false;
    off = value;
    return true;
  }
  bool container(uint32_t id, uint8_t kind, uint32_t& pos, uint32_t& count) const {
    if (!object(id, pos) || data[pos] >> 4 != kind) return false;
    count = data[pos++] & 15;
    if (count != 15) return true;
    if (pos >= offsets || data[pos] >> 4 != 1 || (data[pos] & 15) > 3) return false;
    uint32_t width = 1u << (data[pos++] & 15);
    uint64_t value;
    if (width > offsets - pos || !integer(pos, width, value) || value > UINT32_MAX) return false;
    pos += width;
    count = value;
    return true;
  }
  bool lookup(uint32_t dict, const char* key, uint32_t& result) const {
    uint32_t pos, count;
    if (!container(dict, 0xd, pos, count) || count > 128 || count > (offsets - pos) / (2u * refWidth)) return false;
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
      uint64_t k, v;
      if (!integer(pos + i * refWidth, refWidth, k) || !integer(pos + (count + i) * refWidth, refWidth, v) ||
          k >= objects || v >= objects)
        return false;
      uint32_t str, n;
      if (!container(k, 5, str, n) || n > offsets - str) return false;
      if (n == strlen(key) && !memcmp(data + str, key, n)) {
        if (found) return false;
        found = true;
        result = v;
      }
    }
    return found;
  }
  bool string(uint32_t id, char* out, uint32_t capacity) const {
    uint32_t pos, n, off;
    if (!object(id, off)) return false;
    uint8_t kind = data[off] >> 4;
    if ((kind != 5 && kind != 6) || !container(id, kind, pos, n) || !n || n > 4096 ||
        n > (offsets - pos) / (kind == 6 ? 2u : 1u))
      return false;
    uint32_t written = 0;
    bool clipped = false;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t cp = data[pos + i];
      if (kind == 6) {
        cp = (uint32_t(data[pos + 2 * i]) << 8) | data[pos + 2 * i + 1];
        if (cp >= 0xd800 && cp <= 0xdbff) {
          if (++i >= n) return false;
          uint32_t low = (uint32_t(data[pos + 2 * i]) << 8) | data[pos + 2 * i + 1];
          if (low < 0xdc00 || low > 0xdfff) return false;
          cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        } else if (cp >= 0xdc00 && cp <= 0xdfff)
          return false;
      } else if (cp > 127)
        return false;
      if (cp < 32 || cp == 127) cp = ' ';
      uint8_t encoded[4];
      uint32_t bytes;
      if (cp < 0x80) {
        encoded[0] = cp;
        bytes = 1;
      } else if (cp < 0x800) {
        encoded[0] = 0xc0 | (cp >> 6);
        encoded[1] = 0x80 | (cp & 63);
        bytes = 2;
      } else if (cp < 0x10000) {
        encoded[0] = 0xe0 | (cp >> 12);
        encoded[1] = 0x80 | ((cp >> 6) & 63);
        encoded[2] = 0x80 | (cp & 63);
        bytes = 3;
      } else {
        encoded[0] = 0xf0 | (cp >> 18);
        encoded[1] = 0x80 | ((cp >> 12) & 63);
        encoded[2] = 0x80 | ((cp >> 6) & 63);
        encoded[3] = 0x80 | (cp & 63);
        bytes = 4;
      }
      if (out && !clipped) {
        if (bytes >= capacity - written)
          clipped = true;
        else {
          memcpy(out + written, encoded, bytes);
          written += bytes;
        }
      }
    }
    if (out) out[written] = 0;
    return true;
  }
};
