#pragma once

// Clean-room, wire-compatible implementation of CidVonHighwind/MicroReader's
// serial file-transfer protocol (independent implementation, protocol only —
// no firmware code copied). Goal: interoperate with MicroReader's host tools
// (serial_cmd.py, Calibre plugin) while being leaner/faster.
//
// The protocol logic here is deliberately hardware-free: it talks to the world
// through the SerialTransferHost interface (byte I/O + a file sink + a book
// lister), so it can be unit-tested on the host without Serial/HalStorage. The
// firmware wires those callbacks to logSerial and HalStorage; tests wire fakes.
//
// Wire format (source of truth: MicroReader tools/serial_cmd.py):
//   Command   = MAGIC("CMND") + opcode[1] [+ payload]
//   Upload     = MAGIC("EPUB") + <u16 nameLen> + name + <u32 dataLen> + data
//   <u16>/<u32> are little-endian. Text replies are '\n'-terminated ASCII.
//   Upload: device replies "READY", streams data in 2048-byte chunks, ACKs each
//   chunk with byte 0x06, then reads a trailing <u32 crc32> and replies
//   "OK"/"ERR:...". CRC32 is zlib/IEEE (poly 0xEDB88320).
//   Download   = CMND opcode 'T' + <u16 pathLen> + path. Device replies "READY"
//   then <u32 size> + data in 2048-byte chunks (host 0x06-ACKs each, flow
//   control) + <u32 crc32>, or "ERR:...". (ACK-paced unlike MicroReader's 'T',
//   which streams unpaced; our host tools are the only download clients.)
//   CMND opcodes (1 byte after "CMND"): 'L' list books, 'S' status, 'R' remove,
//   'T' download, 'A' list dir (DIR:/d|/f|name|size|mtime/END), 'W' write file
//   to an arbitrary path (same framing as EPUB upload), 'N' rename/move
//   (<u16>src + <u16>dst), 'K' mkdir. Replies are "OK"/"ERR:..." unless noted.
//   A path or upload name holding a NUL or another control byte (< 0x20) is refused with "ERR:badpath" (an upload's
//   after its <u32 dataLen>, before "READY").

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace serialtransfer {

// One book entry as reported by the `L` (list) command: BOOKS: path|title|author
struct BookEntry {
  std::string path;
  std::string title;
  std::string author;
};

// One entry of a directory listing (CMND 'A'). Files are sent as
// "f|name|size|mtime", directories as "d|name".
struct DirEntry {
  bool isDir = false;
  std::string name;
  uint32_t size = 0;   // bytes (files only)
  uint32_t mtime = 0;  // unix seconds, 0 if unknown
};

// Abstracts everything the protocol needs from the outside world so the state
// machine stays testable. The firmware implements this over logSerial +
// HalStorage; the host test implements it over in-memory buffers.
class SerialTransferHost {
 public:
  virtual ~SerialTransferHost() = default;

  // --- Inbound byte stream (from the serial line) ---
  // Number of bytes currently available to read without blocking.
  virtual size_t available() = 0;
  // Read one byte. Returns -1 if none became available within the timeout.
  virtual int readByte() = 0;
  // Non-destructively look at the byte `i` positions ahead (0 = next byte to be
  // read). Returns -1 if fewer than i+1 bytes are available. Used to probe the
  // 4-byte magic without consuming it, so a non-matching probe leaves the bytes
  // for the caller's own protocol.
  virtual int peek(size_t i) = 0;

  // --- Outbound (to the serial line) ---
  virtual void writeBytes(const uint8_t* data, size_t len) = 0;
  // Convenience: write an ASCII line followed by '\n'.
  void writeLine(const char* s);

  // --- File sink for uploads ---
  // Begin receiving a file at `path`. Returns false if it can't be created.
  virtual bool fileBegin(const std::string& path) = 0;
  // Append a chunk to the in-progress file. Returns false on write failure.
  virtual bool fileWrite(const uint8_t* data, size_t len) = 0;
  // Finish the file. If `keep` is false the (partial) file must be removed.
  virtual void fileEnd(bool keep) = 0;

  // --- File source for downloads (CMND 'T') ---
  // Open `path` for reading. Returns false if it can't be opened; on success
  // sets outSize to the file's byte length (sent to the host before the data).
  virtual bool fileReadBegin(const std::string& path, uint32_t& outSize) = 0;
  // Read up to `len` bytes into `buf`. Returns the number read (0 at EOF).
  virtual size_t fileRead(uint8_t* buf, size_t len) = 0;
  // Finish reading (close the file).
  virtual void fileReadEnd() = 0;

  // --- Directory listing (CMND 'A'), streamed one entry at a time so the
  //     device never builds a vector for a large folder. ---
  // Open `path` for iteration. Returns false if it can't be opened.
  virtual bool listDirBegin(const std::string& path) = 0;
  // Fill `out` with the next entry; returns false when iteration is done.
  virtual bool listDirNext(DirEntry& out) = 0;
  // End iteration (close the directory).
  virtual void listDirEnd() = 0;

  // --- Other file ops ---
  // Rename / move src -> dst. Returns false on failure (CMND 'N').
  virtual bool renameFile(const std::string& src, const std::string& dst) = 0;
  // Create directory (and parents). Returns false on failure (CMND 'K').
  virtual bool makeDir(const std::string& path) = 0;

  // --- Misc device queries used by simple opcodes ---
  virtual std::vector<BookEntry> listBooks() = 0;
  virtual bool removeFile(const std::string& path) = 0;
  virtual std::string statusLine() = 0;  // payload after "STATUS:"
  // Maps an upload name to its destination path on storage (e.g. books root).
  virtual std::string uploadDestination(const std::string& name) = 0;
};

// Drives one poll cycle. Detects the 4-byte magic, dispatches the opcode, and
// for uploads runs the chunked receive + CRC verification. Stateless between
// top-level messages: each poll() handles at most one complete message.
class SerialTransferProtocol {
 public:
  explicit SerialTransferProtocol(SerialTransferHost& host) : host_(host) {}

  // Returns true if a message was recognized and handled this call. Returns
  // false (without consuming bytes beyond the magic probe) when the leading
  // bytes are not one of our magics, so the caller can fall through to its own
  // line-based protocol.
  bool poll();

  // Chunk size mandated by the protocol (one 0x06 ACK per chunk).
  static constexpr size_t kChunkSize = 2048;

  // Upper bound on a length-prefixed path/name field. Real SD paths are well
  // under this; the cap stops a malformed <u16 len> from forcing a huge
  // (failure-prone, abort()-on-OOM under -fno-exceptions) std::string alloc.
  static constexpr uint16_t kMaxPathLen = 512;

 private:
  bool handleCommand();   // after "CMND" consumed
  bool handleUpload();    // after "EPUB" consumed
  bool handleDownload();  // CMND 'T': stream a file out to the host
  bool handleListDir();   // CMND 'A': directory listing
  bool handleWrite();     // CMND 'W': write file to an arbitrary path
  bool handleRename();    // CMND 'N': rename / move
  bool handleMkDir();     // CMND 'K': make directory
  // Shared chunked receive (header already consumed): reads <u32 size> + 2KB
  // chunks (0x06 ACK each) + <u32 crc>, writing to `dest`. Used by EPUB and 'W'.
  bool receiveFile(const std::string& dest);
  bool readExact(uint8_t* buf, size_t len);
  bool readU16LE(uint16_t& out);
  bool readU32LE(uint32_t& out);
  // Read a <u16 len> + bytes field into `out`, rejecting absurd lengths. Returns
  // false on timeout or over-cap length (caller bails; the host's read times out
  // and the device's magic resync recovers).
  bool readLenPrefixed(std::string& out);
  // True, after replying "ERR:badpath", when `path` holds a NUL or another control byte: such a path is refused,
  // not acted on. Callers read the command's every field first, so nothing is left on the wire.
  bool refuseBadPath(const std::string& path);

  SerialTransferHost& host_;
};

// Incremental CRC32 (zlib/IEEE, poly 0xEDB88320), matching zlib.crc32 on the
// host. Seed with 0; feed chunks; the running value is the final CRC.
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);

}  // namespace serialtransfer
