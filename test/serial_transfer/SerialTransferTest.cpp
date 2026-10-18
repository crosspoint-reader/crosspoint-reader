#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "SerialTransfer/SerialTransferProtocol.h"

using serialtransfer::BookEntry;
using serialtransfer::crc32Update;
using serialtransfer::DirEntry;
using serialtransfer::SerialTransferHost;
using serialtransfer::SerialTransferProtocol;

namespace {

// In-memory host: `in` feeds the protocol, `out` captures replies, and the file
// sink records the last uploaded file. Mirrors what the firmware wires to
// logSerial + HalStorage, but with no hardware.
class FakeHost : public SerialTransferHost {
 public:
  std::deque<uint8_t> in;        // bytes the device "receives"
  std::vector<uint8_t> out;      // bytes the device "sends"
  std::vector<BookEntry> books;  // what listBooks() returns

  // Captured upload state.
  std::string lastPath;
  std::vector<uint8_t> lastData;
  bool fileKept = false;
  bool failCreate = false;

  std::vector<std::string> removed;
  bool removeResult = true;

  // Captured download (read) state.
  std::vector<uint8_t> downloadSource;  // file content fileRead() serves
  std::string lastReadPath;
  size_t readPos = 0;
  bool failOpen = false;

  // Directory listing / file-op state.
  std::vector<DirEntry> dirEntries;
  std::string lastListPath;
  size_t dirPos = 0;
  bool failListDir = false;
  std::string renSrc, renDst;
  bool renameResult = true;
  std::string mkdirPath;
  bool mkdirResult = true;

  // -- inbound --
  size_t available() override { return in.size(); }
  int readByte() override {
    if (in.empty()) return -1;
    int b = in.front();
    in.pop_front();
    return b;
  }
  int peek(size_t i) override {
    if (i >= in.size()) return -1;
    return in[i];
  }

  // -- outbound --
  void writeBytes(const uint8_t* data, size_t len) override { out.insert(out.end(), data, data + len); }

  // -- file sink --
  bool fileBegin(const std::string& path) override {
    if (failCreate) return false;
    lastPath = path;
    lastData.clear();
    fileKept = false;
    return true;
  }
  bool fileWrite(const uint8_t* data, size_t len) override {
    lastData.insert(lastData.end(), data, data + len);
    return true;
  }
  void fileEnd(bool keep) override { fileKept = keep; }

  // -- file source (download) --
  bool fileReadBegin(const std::string& path, uint32_t& outSize) override {
    if (failOpen) return false;
    lastReadPath = path;
    readPos = 0;
    outSize = static_cast<uint32_t>(downloadSource.size());
    return true;
  }
  size_t fileRead(uint8_t* buf, size_t len) override {
    const size_t n = std::min(len, downloadSource.size() - readPos);
    std::copy(downloadSource.begin() + readPos, downloadSource.begin() + readPos + n, buf);
    readPos += n;
    return n;
  }
  void fileReadEnd() override {}

  // -- directory listing + file ops --
  bool listDirBegin(const std::string& path) override {
    if (failListDir) return false;
    lastListPath = path;
    dirPos = 0;
    return true;
  }
  bool listDirNext(DirEntry& out) override {
    if (dirPos >= dirEntries.size()) return false;
    out = dirEntries[dirPos++];
    return true;
  }
  void listDirEnd() override {}
  bool renameFile(const std::string& src, const std::string& dst) override {
    renSrc = src;
    renDst = dst;
    return renameResult;
  }
  bool makeDir(const std::string& path) override {
    mkdirPath = path;
    return mkdirResult;
  }

  // -- queries --
  std::vector<BookEntry> listBooks() override { return books; }
  bool removeFile(const std::string& path) override {
    removed.push_back(path);
    return removeResult;
  }
  std::string statusLine() override { return "heap=12345"; }
  std::string uploadDestination(const std::string& name) override { return "/books/" + name; }

  // -- helpers for building the input stream --
  void push(const std::string& s) {
    for (char c : s) in.push_back(static_cast<uint8_t>(c));
  }
  void push(const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; ++i) in.push_back(d[i]);
  }
  void pushU16(uint16_t v) {
    in.push_back(v & 0xFF);
    in.push_back((v >> 8) & 0xFF);
  }
  void pushU32(uint32_t v) {
    in.push_back(v & 0xFF);
    in.push_back((v >> 8) & 0xFF);
    in.push_back((v >> 16) & 0xFF);
    in.push_back((v >> 24) & 0xFF);
  }

  std::string outStr() const { return std::string(out.begin(), out.end()); }
  size_t ackCount() const {
    size_t n = 0;
    for (uint8_t b : out)
      if (b == 0x06) ++n;
    return n;
  }
};

uint32_t zlibCrc(const std::vector<uint8_t>& d) { return crc32Update(0, d.data(), d.size()); }

}  // namespace

// CRC32 must match zlib.crc32 for known vectors (the host tool uses zlib).
TEST(SerialTransferCrc, KnownVectors) {
  // zlib.crc32(b"") == 0
  EXPECT_EQ(crc32Update(0, nullptr, 0), 0u);
  // zlib.crc32(b"123456789") == 0xCBF43926
  const std::string s = "123456789";
  EXPECT_EQ(crc32Update(0, reinterpret_cast<const uint8_t*>(s.data()), s.size()), 0xCBF43926u);
}

TEST(SerialTransferCrc, IncrementalEqualsOneShot) {
  std::vector<uint8_t> data;
  for (int i = 0; i < 5000; ++i) data.push_back(static_cast<uint8_t>(i * 7 + 1));
  uint32_t whole = crc32Update(0, data.data(), data.size());
  uint32_t split = 0;
  split = crc32Update(split, data.data(), 2048);
  split = crc32Update(split, data.data() + 2048, 2048);
  split = crc32Update(split, data.data() + 4096, data.size() - 4096);
  EXPECT_EQ(whole, split);
}

// Non-magic leading bytes must not be consumed-as-handled; poll() returns false
// so the caller can fall through to its line-based protocol.
TEST(SerialTransferDispatch, IgnoresForeignMagic) {
  FakeHost h;
  const std::string line = "CMD:SCREENSHOT\n";
  h.push(line);  // the firmware's existing line protocol
  SerialTransferProtocol proto(h);
  EXPECT_FALSE(proto.poll());
  EXPECT_TRUE(h.out.empty());
  // Crucial: a non-matching probe must NOT consume any bytes, so the caller's
  // line handler still sees the full "CMD:SCREENSHOT\n".
  EXPECT_EQ(h.in.size(), line.size());
}

// The USB Transfer screen answers the firmware's own CMD: lines between transfer messages. It can
// tell one from a transfer message by its first four bytes, without consuming anything, so a
// message that only starts like one still reaches the protocol whole.
TEST(SerialTransferDispatch, RecognisesATextCommandWithoutConsumingIt) {
  FakeHost h;
  h.push("CMD:STATE\n");
  EXPECT_TRUE(serialtransfer::textCommandAhead(h));
  EXPECT_EQ(h.in.size(), 10u);

  FakeHost transfer;
  transfer.push("CMNDS");
  EXPECT_FALSE(serialtransfer::textCommandAhead(transfer)) << "CMND is the protocol's magic, not CMD:";

  FakeHost partial;
  partial.push("CMD");
  EXPECT_FALSE(serialtransfer::textCommandAhead(partial)) << "not decided before four bytes are in";
  EXPECT_EQ(partial.in.size(), 3u);
}

// A magic that only partially arrived must not be consumed; poll() waits.
TEST(SerialTransferDispatch, PartialMagicNotConsumed) {
  FakeHost h;
  h.push("CM");  // only 2 of 4 magic bytes present
  SerialTransferProtocol proto(h);
  EXPECT_FALSE(proto.poll());
  EXPECT_EQ(h.in.size(), 2u);  // nothing consumed
}

TEST(SerialTransferDispatch, ListBooks) {
  FakeHost h;
  h.books = {{"/books/a.epub", "Alpha", "AuthA"}, {"/books/b.epub", "Beta", "AuthB"}};
  h.push("CMNDL");
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.outStr(), "BOOKS:\n/books/a.epub|Alpha|AuthA\n/books/b.epub|Beta|AuthB\nEND\n");
}

TEST(SerialTransferDispatch, RemoveFile) {
  FakeHost h;
  const std::string path = "/books/old.epub";
  h.push("CMNDR");
  h.pushU16(static_cast<uint16_t>(path.size()));
  h.push(path);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  ASSERT_EQ(h.removed.size(), 1u);
  EXPECT_EQ(h.removed[0], path);
  EXPECT_EQ(h.outStr(), "OK\n");
}

TEST(SerialTransferDispatch, RemoveFileFailure) {
  FakeHost h;
  h.removeResult = false;
  const std::string path = "/books/x.epub";
  h.push("CMNDR");
  h.pushU16(static_cast<uint16_t>(path.size()));
  h.push(path);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.outStr(), "ERR:remove\n");
}

TEST(SerialTransferDispatch, Status) {
  FakeHost h;
  h.push("CMNDS");
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.outStr(), "STATUS:heap=12345\n");
}

TEST(SerialTransferDispatch, UnsupportedOpcode) {
  FakeHost h;
  h.push("CMNDX");  // bench opcode we don't implement
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.outStr(), "ERR:unsupported\n");
}

// Full upload round-trip: header, READY, chunked data with per-chunk ACK,
// trailing CRC, final OK. Data spans multiple chunks plus a partial.
TEST(SerialTransferUpload, RoundTripMultiChunk) {
  FakeHost h;
  const std::string name = "book.epub";
  std::vector<uint8_t> data;
  for (int i = 0; i < 2048 * 2 + 100; ++i) data.push_back(static_cast<uint8_t>((i * 31 + 5) & 0xFF));

  h.push("EPUB");
  h.pushU16(static_cast<uint16_t>(name.size()));
  h.push(name);
  h.pushU32(static_cast<uint32_t>(data.size()));
  h.push(data.data(), data.size());
  h.pushU32(zlibCrc(data));

  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());

  EXPECT_EQ(h.lastPath, "/books/book.epub");
  EXPECT_EQ(h.lastData, data);
  EXPECT_TRUE(h.fileKept);
  // 3 chunks (2048 + 2048 + 100) => 3 ACKs.
  EXPECT_EQ(h.ackCount(), 3u);
  // Reply ends with READY then OK (ACK bytes interleaved as 0x06).
  const std::string s = h.outStr();
  EXPECT_NE(s.find("READY\n"), std::string::npos);
  EXPECT_NE(s.find("OK\n"), std::string::npos);
}

TEST(SerialTransferUpload, BadCrcRejectsAndDeletes) {
  FakeHost h;
  const std::string name = "bad.epub";
  std::vector<uint8_t> data = {1, 2, 3, 4, 5};
  h.push("EPUB");
  h.pushU16(static_cast<uint16_t>(name.size()));
  h.push(name);
  h.pushU32(static_cast<uint32_t>(data.size()));
  h.push(data.data(), data.size());
  h.pushU32(zlibCrc(data) ^ 0xFFFFFFFFu);  // wrong CRC

  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_FALSE(h.fileKept);  // partial file removed
  EXPECT_NE(h.outStr().find("ERR:crc\n"), std::string::npos);
}

TEST(SerialTransferUpload, CreateFailureReportsErr) {
  FakeHost h;
  h.failCreate = true;
  const std::string name = "x.epub";
  h.push("EPUB");
  h.pushU16(static_cast<uint16_t>(name.size()));
  h.push(name);
  h.pushU32(0);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.outStr(), "ERR:create\n");
}

TEST(SerialTransferUpload, TruncatedStreamTimesOut) {
  FakeHost h;
  const std::string name = "t.epub";
  h.push("EPUB");
  h.pushU16(static_cast<uint16_t>(name.size()));
  h.push(name);
  h.pushU32(100);   // claims 100 bytes...
  h.push("short");  // ...but only 5 arrive, then stream ends (readByte -> -1)
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_FALSE(h.fileKept);
  EXPECT_NE(h.outStr().find("ERR:io\n"), std::string::npos);
}

namespace {
// Build the expected download reply: "READY\n" + <u32 size LE> + data + <u32 crc LE>.
std::vector<uint8_t> expectedDownload(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> e;
  const char* ready = "READY\n";
  e.insert(e.end(), ready, ready + 6);
  auto u32 = [&](uint32_t v) {
    e.push_back(v & 0xFF);
    e.push_back((v >> 8) & 0xFF);
    e.push_back((v >> 16) & 0xFF);
    e.push_back((v >> 24) & 0xFF);
  };
  u32(static_cast<uint32_t>(data.size()));
  e.insert(e.end(), data.begin(), data.end());
  u32(zlibCrc(data));
  return e;
}
}  // namespace

// Full download (CMND 'T') round-trip spanning multiple chunks plus a partial.
TEST(SerialTransferDownload, RoundTripMultiChunk) {
  FakeHost h;
  for (int i = 0; i < 2048 * 2 + 100; ++i) h.downloadSource.push_back(static_cast<uint8_t>((i * 17 + 3) & 0xFF));

  const std::string path = "/books/grab.epub";
  h.push("CMNDT");
  h.pushU16(static_cast<uint16_t>(path.size()));
  h.push(path);
  // Download is ACK-paced: the device reads one 0x06 per 2048-byte chunk.
  const size_t chunks = (h.downloadSource.size() + 2047) / 2048;  // 3 here
  for (size_t i = 0; i < chunks; ++i) h.in.push_back(0x06);

  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.lastReadPath, path);
  EXPECT_EQ(h.out, expectedDownload(h.downloadSource));
}

// If the host never ACKs a chunk, the device aborts and sends no trailing CRC.
TEST(SerialTransferDownload, NoAckAborts) {
  FakeHost h;
  for (int i = 0; i < 100; ++i) h.downloadSource.push_back(static_cast<uint8_t>(i));
  const std::string path = "/x.bin";
  h.push("CMNDT");
  h.pushU16(static_cast<uint16_t>(path.size()));
  h.push(path);
  // no 0x06 ACK provided -> readByte() returns -1 after the first chunk
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  // READY + size + the chunk data, but NO 4-byte CRC at the end.
  EXPECT_EQ(h.out.size(), 6u + 4u + 100u);
}

TEST(SerialTransferDownload, EmptyFile) {
  FakeHost h;  // downloadSource left empty
  const std::string path = "/empty.bin";
  h.push("CMNDT");
  h.pushU16(static_cast<uint16_t>(path.size()));
  h.push(path);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  // READY\n + size(0) + crc(0)
  const std::vector<uint8_t> expected = {'R', 'E', 'A', 'D', 'Y', '\n', 0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(h.out, expected);
}

TEST(SerialTransferDownload, OpenFailure) {
  FakeHost h;
  h.failOpen = true;
  const std::string path = "/nope.epub";
  h.push("CMNDT");
  h.pushU16(static_cast<uint16_t>(path.size()));
  h.push(path);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.outStr(), "ERR:fopen\n");
}

// --- Directory listing (CMND 'A') ---
TEST(SerialTransferDispatch, ListDir) {
  FakeHost h;
  h.dirEntries = {
      {true, "sub", 0, 0},
      {false, "a.epub", 1234, 1700000000u},
      {false, "b.txt", 7, 0},
  };
  const std::string path = "/books";
  h.push("CMNDA");
  h.pushU16(static_cast<uint16_t>(path.size()));
  h.push(path);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.lastListPath, path);
  EXPECT_EQ(h.outStr(), "DIR:/books\nd|sub\nf|a.epub|1234|1700000000\nf|b.txt|7|0\nEND\n");
}

TEST(SerialTransferDispatch, ListDirOpenFailure) {
  FakeHost h;
  h.failListDir = true;
  const std::string path = "/nope";
  h.push("CMNDA");
  h.pushU16(static_cast<uint16_t>(path.size()));
  h.push(path);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.outStr(), "ERR:opendir\n");
}

// --- Write to arbitrary path (CMND 'W'), shares the chunked receive with EPUB ---
TEST(SerialTransferWrite, RoundTripMultiChunk) {
  FakeHost h;
  std::vector<uint8_t> data;
  for (int i = 0; i < 2048 + 50; ++i) data.push_back(static_cast<uint8_t>((i * 13 + 7) & 0xFF));
  const std::string path = "/sleep/cover.bmp";

  h.push("CMNDW");
  h.pushU16(static_cast<uint16_t>(path.size()));
  h.push(path);
  h.pushU32(static_cast<uint32_t>(data.size()));
  h.push(data.data(), data.size());
  h.pushU32(zlibCrc(data));

  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.lastPath, path);  // written to the exact path, not /books/
  EXPECT_EQ(h.lastData, data);
  EXPECT_TRUE(h.fileKept);
  EXPECT_EQ(h.ackCount(), 2u);  // 2048 + 50 => 2 chunks
  EXPECT_NE(h.outStr().find("READY\n"), std::string::npos);
  EXPECT_NE(h.outStr().find("OK\n"), std::string::npos);
}

// --- Rename / move (CMND 'N') ---
TEST(SerialTransferDispatch, Rename) {
  FakeHost h;
  const std::string src = "/a/old.epub", dst = "/b/new.epub";
  h.push("CMNDN");
  h.pushU16(static_cast<uint16_t>(src.size()));
  h.push(src);
  h.pushU16(static_cast<uint16_t>(dst.size()));
  h.push(dst);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.renSrc, src);
  EXPECT_EQ(h.renDst, dst);
  EXPECT_EQ(h.outStr(), "OK\n");
}

TEST(SerialTransferDispatch, RenameFailure) {
  FakeHost h;
  h.renameResult = false;
  const std::string src = "/a", dst = "/b";
  h.push("CMNDN");
  h.pushU16(static_cast<uint16_t>(src.size()));
  h.push(src);
  h.pushU16(static_cast<uint16_t>(dst.size()));
  h.push(dst);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.outStr(), "ERR:rename_failed\n");
}

// --- Make directory (CMND 'K') ---
TEST(SerialTransferDispatch, MkDir) {
  FakeHost h;
  const std::string path = "/new/folder";
  h.push("CMNDK");
  h.pushU16(static_cast<uint16_t>(path.size()));
  h.push(path);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.mkdirPath, path);
  EXPECT_EQ(h.outStr(), "OK\n");
}

TEST(SerialTransferDispatch, MkDirFailure) {
  FakeHost h;
  h.mkdirResult = false;
  const std::string path = "/x";
  h.push("CMNDK");
  h.pushU16(static_cast<uint16_t>(path.size()));
  h.push(path);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.outStr(), "ERR:mkdir_failed\n");
}

// --- Paths carrying a NUL or another control byte ----------------------------
//
// On the X3 the USB serial link twice delivered a 0 in place of a name's byte. A NUL ends the C string the file
// system is given, so a rename to "/.crosspoint/fonts/BookerlyTTF.off" created a folder named "Booker", and a
// remove would delete whatever the truncated path names. Such a path is refused instead.

namespace {

const std::string kNul(1, '\0');

void pushPathCommand(FakeHost& h, const char* command, const std::string& path) {
  h.push(command);
  h.pushU16(static_cast<uint16_t>(path.size()));
  h.push(path);
}

}  // namespace

TEST(SerialTransferBadPath, RenameRefusesADestinationWithANul) {
  FakeHost h;
  const std::string src = "/.crosspoint/fonts/BookerlyTTF";
  const std::string dst = "/.crosspoint/fonts/Booker" + kNul + "yTTF.off";  // as received on the X3
  pushPathCommand(h, "CMNDN", src);
  h.pushU16(static_cast<uint16_t>(dst.size()));
  h.push(dst);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_TRUE(h.renSrc.empty());  // nothing renamed
  EXPECT_EQ(h.outStr(), "ERR:badpath\n");
}

TEST(SerialTransferBadPath, RenameRefusesASourceWithANul) {
  FakeHost h;
  pushPathCommand(h, "CMNDN", "/books/A" + kNul + "B.epub");
  h.pushU16(6);
  h.push("/b.txt");
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_TRUE(h.renSrc.empty());
  EXPECT_EQ(h.outStr(), "ERR:badpath\n");
}

TEST(SerialTransferBadPath, RemoveRefusesAPathWithANul) {
  FakeHost h;
  pushPathCommand(h, "CMNDR", "/books/A" + kNul + "B.epub");  // would remove "/books/A"
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_TRUE(h.removed.empty());
  EXPECT_EQ(h.outStr(), "ERR:badpath\n");
}

TEST(SerialTransferBadPath, MkDirRefusesAPathWithAControlByte) {
  FakeHost h;
  pushPathCommand(h, "CMNDK", "/notes\x01x");
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_TRUE(h.mkdirPath.empty());
  EXPECT_EQ(h.outStr(), "ERR:badpath\n");
}

TEST(SerialTransferBadPath, WriteRefusesAPathWithANul) {
  FakeHost h;
  pushPathCommand(h, "CMNDW", "/books/x" + kNul + ".epub");
  h.pushU32(4);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_TRUE(h.lastPath.empty());  // no file created
  EXPECT_EQ(h.outStr(), "ERR:badpath\n");
}

TEST(SerialTransferBadPath, EpubUploadRefusesANameWithANul) {
  FakeHost h;
  const std::string name = "Book" + kNul + ".epub";
  h.push("EPUB");
  h.pushU16(static_cast<uint16_t>(name.size()));
  h.push(name);
  h.pushU32(4);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_TRUE(h.lastPath.empty());
  EXPECT_EQ(h.outStr(), "ERR:badpath\n");
}

TEST(SerialTransferBadPath, ListAndDownloadRefuseAPathWithANul) {
  FakeHost list;
  pushPathCommand(list, "CMNDA", "/books" + kNul);
  SerialTransferProtocol listProto(list);
  EXPECT_TRUE(listProto.poll());
  EXPECT_TRUE(list.lastListPath.empty());
  EXPECT_EQ(list.outStr(), "ERR:badpath\n");

  FakeHost download;
  pushPathCommand(download, "CMNDT", "/books/a" + kNul + ".epub");
  SerialTransferProtocol downloadProto(download);
  EXPECT_TRUE(downloadProto.poll());
  EXPECT_TRUE(download.lastReadPath.empty());
  EXPECT_EQ(download.outStr(), "ERR:badpath\n");
}

TEST(SerialTransferBadPath, UnicodeNamesAreStillAccepted) {
  FakeHost h;
  const std::string path = "/books/Caf\xC3\xA9 \xE4\xB8\xAD.epub";  // UTF-8: é and a CJK character
  pushPathCommand(h, "CMNDR", path);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  ASSERT_EQ(1u, h.removed.size());
  EXPECT_EQ(path, h.removed[0]);
  EXPECT_EQ(h.outStr(), "OK\n");
}

TEST(SerialTransferBadPath, RenameRefusingItsSourceStillConsumesTheDestination) {
  // A destination left on the wire would be scanned for a magic: "/EPUB/..." would start an upload.
  FakeHost h;
  pushPathCommand(h, "CMNDN", "/books/A" + kNul + "B.epub");
  const std::string dst = "/EPUB/b.epub";
  h.pushU16(static_cast<uint16_t>(dst.size()));
  h.push(dst);
  SerialTransferProtocol proto(h);
  EXPECT_TRUE(proto.poll());
  EXPECT_EQ(h.outStr(), "ERR:badpath\n");
  EXPECT_EQ(0, h.available());
}
