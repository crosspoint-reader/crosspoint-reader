#pragma once

#include <strings.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "network/ProtectedPaths.h"

// File transfer over the USB serial console (LS, GET, PUT, MKDIR, RM in
// SerialCommand.h). Pure, so the path rules and the framing are host-tested;
// SerialFileCommands carries the commands out.
//
// Binary data moves in frames, each a 12-byte header and up to CHUNK_SIZE bytes:
//   0xFE 0x5A | offset u32 LE | length u16 LE | crc u32 LE | payload
// The CRC-32 (zlib's) covers the offset and length fields, then the payload, so
// a frame cannot be applied at the wrong place. 0xFE never occurs in UTF-8, so a
// reader skips log text between frames by scanning for it.
//
// PUT: the host waits for "PUT_READY <chunk>", then sends frames. The reader
// writes each to <path>.part and answers "PUT_ACK <next offset>", or
// "PUT_NAK <next offset> <reason>" for a damaged or unexpected frame; either way
// the host continues from the offset given. Once every byte has arrived the
// reader checks the whole-file CRC and renames the part file over the path.
// The reader holds the host off while it writes (HalSerialBulkRead), so the host
// may send the next frame before the last one is acknowledged.
//
// A frame with offset CANCEL_OFFSET and no payload ends a PUT at once ("cancelled"),
// so a host that gives up need not wait for the reader's timeout.
//
// GET: "CMD_OK GET <size>", then frames from the requested offset to the end,
// with no acknowledgements. The reader stops with "GET_ABORT <reason>" when a
// read fails, the cable goes, or the host sends anything (such as its next command).
namespace serialxfer {

inline constexpr uint8_t FRAME_MAGIC[2] = {0xFE, 0x5A};
inline constexpr size_t FRAME_HEADER_SIZE = 12;
inline constexpr size_t CHUNK_SIZE = 4096;
inline constexpr size_t MAX_PATH_LENGTH = 255;
// A console line long enough for "CMD:PUT REPLACE <size> <crc> " and the longest path.
// MV's two paths share it, so together they may take MAX_LINE_LENGTH - 9 bytes
// ("CMD:MV ", the bar, and a terminal's carriage return).
inline constexpr size_t MAX_LINE_LENGTH = 320;
inline constexpr uint32_t CANCEL_OFFSET = 0xFFFFFFFF;

struct FrameHeader {
  uint32_t offset = 0;
  uint16_t length = 0;
  uint32_t crc = 0;
};

namespace detail {

constexpr void putLe(uint8_t* out, uint32_t value, int bytes) {
  for (int i = 0; i < bytes; i++) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

constexpr uint32_t getLe(const uint8_t* in, int bytes) {
  uint32_t value = 0;
  for (int i = 0; i < bytes; i++) value |= static_cast<uint32_t>(in[i]) << (8 * i);
  return value;
}

}  // namespace detail

constexpr void encodeFrameHeader(const FrameHeader& header, uint8_t* out) {
  out[0] = FRAME_MAGIC[0];
  out[1] = FRAME_MAGIC[1];
  detail::putLe(out + 2, header.offset, 4);
  detail::putLe(out + 6, header.length, 2);
  detail::putLe(out + 8, header.crc, 4);
}

// False when the bytes are not a frame header (wrong magic, oversized payload).
constexpr bool decodeFrameHeader(const uint8_t* in, FrameHeader& header) {
  if (in[0] != FRAME_MAGIC[0] || in[1] != FRAME_MAGIC[1]) return false;
  header.offset = detail::getLe(in + 2, 4);
  header.length = static_cast<uint16_t>(detail::getLe(in + 6, 2));
  header.crc = detail::getLe(in + 8, 4);
  return header.length <= CHUNK_SIZE;
}

// The header bytes the frame CRC covers before the payload (offset and length).
constexpr const uint8_t* crcCoveredHeader(const uint8_t* header) { return header + 2; }
inline constexpr size_t CRC_COVERED_HEADER_SIZE = 6;

// Whether a received PUT frame continues the file. Returns nullptr to accept it,
// else the reason for the NAK; the host then resends from expectedOffset.
constexpr const char* checkFrame(const FrameHeader& header, uint32_t computedCrc, uint32_t expectedOffset,
                                 uint32_t totalSize) {
  if (header.offset != expectedOffset) return "offset";
  if (header.length == 0 || header.length > totalSize - expectedOffset) return "length";
  if (header.crc != computedCrc) return "crc";
  return nullptr;
}

// The first bytes of an uploaded font, checked like the web upload does.
constexpr bool fontMagicOk(const uint8_t* data, size_t size, bool vector) {
  if (size < 8) return false;
  if (!vector) {
    constexpr uint8_t cpfont[8] = {'C', 'P', 'F', 'O', 'N', 'T', 0, 0};
    for (size_t i = 0; i < 8; i++) {
      if (data[i] != cpfont[i]) return false;
    }
    return true;
  }
  // sfnt versions: 0x00010000 (TrueType), "OTTO" (CFF), "true" (Apple).
  const auto is = [&](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return data[0] == a && data[1] == b && data[2] == c && data[3] == d;
  };
  return is(0x00, 0x01, 0x00, 0x00) || is('O', 'T', 'T', 'O') || is('t', 'r', 'u', 'e');
}

// --- Paths ---------------------------------------------------------------------------------

enum class Access { Read, Write, Remove };

// File: an ordinary file or folder. FontFamily / FontFile: /.fonts/<family>[/<file>]
// (or /fonts/...), which go through FontInstaller so fonts land where the web upload
// puts them and the font list is refreshed.
enum class PathKind { Invalid, File, FontFamily, FontFile };

struct PathCheck {
  PathKind kind = PathKind::Invalid;
  const char* error = "";
  std::string_view family;    // FontFamily, FontFile
  std::string_view fileName;  // FontFile
};

namespace detail {

inline bool equalsNoCase(std::string_view a, const char* b) {
  const size_t n = std::char_traits<char>::length(b);
  return a.size() == n && strncasecmp(a.data(), b, n) == 0;
}

// A name SdFat would store or open as some other name: FAT-invalid characters,
// leading spaces and trailing dots or spaces (dropped when opened), and 8.3 aliases
// ("CROSSP~1" opens ".crosspoint").
inline const char* nameProblem(std::string_view name) {
  if (name.empty() || name == "." || name == "..") return "empty, . or .. in the path";
  for (const char c : name) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F ||
        std::string_view("\\:*?\"<>|").find(c) != std::string_view::npos) {
      return "a name has a character FAT does not allow";
    }
  }
  if (name.front() == ' ' || name.back() == ' ' || name.back() == '.') {
    return "a name starts with a space or ends with a space or dot";
  }
  if (protectedpaths::isShortAlias(name.data(), name.data() + name.size())) return "8.3 short names are not accepted";
  return nullptr;
}

inline bool isFontsRoot(std::string_view name) { return equalsNoCase(name, ".fonts") || equalsNoCase(name, "fonts"); }

inline PathCheck fail(const char* error) {
  PathCheck check;
  check.error = error;
  return check;
}

}  // namespace detail

// Which paths each console file command may touch. Reads (LS, GET) see the whole
// card except the credential stores, like the web download. Writes (PUT, MKDIR) stay
// in the visible part of the card (no protectedpaths::isHiddenName folder or file,
// WebDAV's rule), plus font files under a fonts folder. Removal also reaches hidden items, as the
// web file manager's delete does, but never a credential store.
inline PathCheck checkPath(std::string_view path, Access access) {
  using detail::fail;
  if (path.empty() || path.front() != '/') return fail("expects an absolute path");
  if (path.size() > MAX_PATH_LENGTH) return fail("path too long");
  if (path == "/" && access != Access::Read) return fail("not the root folder");

  std::string_view parts[3];
  size_t count = 0;
  for (size_t start = 1; start <= path.size();) {
    size_t end = path.find('/', start);
    if (end == std::string_view::npos) end = path.size();
    const std::string_view name = path.substr(start, end - start);
    if (access == Access::Read) {
      if (name.empty() && end == path.size() && count == 0) break;  // "/" itself
      if (name.empty() || name == "." || name == "..") return fail("empty, . or .. in the path");
    } else if (const char* problem = detail::nameProblem(name)) {
      return fail(problem);
    } else if (!(count == 0 && detail::isFontsRoot(name)) && protectedpaths::isHiddenName(name)) {
      if (name.front() != '.') return fail("protected folder");
      if (access == Access::Write) return fail("hidden folders are not writable");
    }
    if (count < 3) parts[count] = name;
    count++;
    start = end + 1;
  }

  if (protectedpaths::isSensitivePath(std::string(path).c_str())) return fail("protected file");

  PathCheck check;
  if (access == Access::Read) {
    check.kind = PathKind::File;
    return check;
  }

  if (detail::isFontsRoot(parts[0])) {
    if (count == 2 && access == Access::Remove) {
      check.kind = PathKind::FontFamily;
    } else if (count == 3) {
      check.kind = PathKind::FontFile;
      check.fileName = parts[2];
    } else {
      return fail("fonts go in /.fonts/<family>/<file>");
    }
    check.family = parts[1];
    return check;
  }
  check.kind = PathKind::File;
  return check;
}

// Whether two paths end in the same file extension, ignoring case. A book's
// reading state only moves with it while it stays the same kind of book.
inline bool sameExtension(std::string_view a, std::string_view b) {
  const auto extension = [](std::string_view path) {
    const std::string_view name = path.substr(path.rfind('/') + 1);
    const size_t dot = name.rfind('.');
    return dot == std::string_view::npos || dot == 0 ? std::string_view{} : name.substr(dot);
  };
  const std::string_view x = extension(a);
  const std::string_view y = extension(b);
  return x.size() == y.size() && (x.empty() || strncasecmp(x.data(), y.data(), x.size()) == 0);
}

}  // namespace serialxfer
