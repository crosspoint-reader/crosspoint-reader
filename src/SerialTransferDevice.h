#pragma once

// Firmware-side adapter that binds the hardware-free SerialTransferProtocol to
// the real device: logSerial (USB-CDC / HWCDC) for byte I/O and HalStorage for
// the file sink, with cached book metadata from RecentBooksStore for the `L`
// listing.
//
// Owned by SerialTransferActivity, which calls poll() from loop() while it owns
// the serial input (Activity::ownsSerialInput()). Living outside the protocol
// library keeps lib/SerialTransfer host-testable (no Arduino/HalStorage
// dependency).
//
// Wire-compatible with CidVonHighwind/MicroReader's serial protocol; see
// lib/SerialTransfer/SerialTransfer/SerialTransferProtocol.h for attribution.

#include <HalStorage.h>
#include <SerialTransfer/SerialTransferProtocol.h>

#include <deque>

// Implements the protocol's host interface over real hardware. One instance,
// reused across loop() iterations; per-upload buffers are owned here.
class SerialTransferDevice : public serialtransfer::SerialTransferHost {
 public:
  // Status sink: called with short human-readable strings at the start/end of
  // each operation (e.g. "Receiving 'book.epub'"). The activity renders these
  // on-screen so the user (and on-device debugging) gets feedback even though
  // LOG_* is muted on the wire during a transfer. Plain function pointer + ctx
  // to avoid std::function heap/binary bloat (see CLAUDE.md).
  using StatusFn = void (*)(void* ctx, const char* msg);
  void setStatusCallback(StatusFn fn, void* ctx) {
    statusFn_ = fn;
    statusCtx_ = ctx;
  }

  SerialTransferDevice();

  // Called once per loop(). Returns true if a transfer message was handled.
  bool poll();

  // Slide the magic-probe window forward by one byte to resync after a
  // non-matching 4-byte window (garbage / desync). The activity calls this when
  // poll() returns false while >= 4 bytes are buffered, so a stray byte can't
  // wedge the probe on the same non-matching window forever.
  void dropByte();

  // True when the bytes ahead are a "CMD:" text command rather than a transfer message; see
  // serialtransfer::textCommandAhead(). The activity then reads the line with readByte().
  bool textCommandAhead() { return serialtransfer::textCommandAhead(*this); }

  // SerialTransferHost
  size_t available() override;
  int readByte() override;
  int peek(size_t i) override;
  void writeBytes(const uint8_t* data, size_t len) override;
  bool fileBegin(const std::string& path) override;
  bool fileWrite(const uint8_t* data, size_t len) override;
  void fileEnd(bool keep) override;
  bool fileReadBegin(const std::string& path, uint32_t& outSize) override;
  size_t fileRead(uint8_t* buf, size_t len) override;
  void fileReadEnd() override;
  bool listDirBegin(const std::string& path) override;
  bool listDirNext(serialtransfer::DirEntry& out) override;
  void listDirEnd() override;
  bool renameFile(const std::string& src, const std::string& dst) override;
  bool makeDir(const std::string& path) override;
  std::vector<serialtransfer::BookEntry> listBooks() override;
  bool removeFile(const std::string& path) override;
  std::string statusLine() override;
  std::string uploadDestination(const std::string& name) override;

 private:
  bool flushCoalesceBuffer();
  void emitStatus(const char* fmt, ...);

  serialtransfer::SerialTransferProtocol protocol_;

  // Holds bytes peek() pulled from serial while probing the 4-byte magic.
  // readByte()/available() serve these before touching the live serial.
  std::deque<uint8_t> lookahead_;

  // In-progress upload state.
  HalFile uploadFile_;
  std::string uploadPath_;
  std::string uploadName_;  // basename of uploadPath_, for status messages
  bool uploadOpen_ = false;

  // Coalesce small protocol chunks into larger SD writes so the per-2048-byte
  // ACK isn't gated on a fresh FAT/block write each time. Sized to a few chunks.
  static constexpr size_t kCoalesceSize = 16 * 1024;
  std::vector<uint8_t> coalesce_;

  // Timing for the post-upload throughput status/log.
  uint32_t uploadBytes_ = 0;
  unsigned long uploadStartMs_ = 0;

  // In-progress download (CMND 'T') state.
  HalFile downloadFile_;
  std::string downloadName_;
  uint32_t downloadBytes_ = 0;
  unsigned long downloadStartMs_ = 0;

  // In-progress directory listing (CMND 'A') handle.
  HalFile dirHandle_;

  StatusFn statusFn_ = nullptr;
  void* statusCtx_ = nullptr;
};
