#include "SerialFileCommands.h"

#ifdef ENABLE_SERIAL_FILES

#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalSerialBulkRead.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_rom_crc.h>
#include <strings.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "SdCardFontSystem.h"
#include "SerialConsole.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "util/BookCacheUtils.h"
#include "util/SerialCommand.h"
#include "util/SerialTransfer.h"
#include "util/TaskWatchdog.h"

namespace {

using serialconsole::replyErr;
using serialconsole::replyOk;
using serialconsole::sendLine;
using serialxfer::Access;
using serialxfer::PathKind;

// A frame arrives in one burst; a pause this long inside one means the host stopped mid-frame.
constexpr uint32_t FRAME_GAP_MS = 100;
// No frame for this long: the host has gone, so the transfer is dropped.
constexpr uint32_t HOST_TIMEOUT_MS = 5000;
// LFN names reach 255 UTF-16 units; the web file list reads them into 500 bytes too.
constexpr size_t NAME_BUFFER_SIZE = 500;

// Changes wait for Home, where no screen keeps a book or font file open, and the
// transfer holds the main loop, so no screen can open one until it ends. Home's
// render task still opens recent books and writes their cover thumbnails into the
// book cache under the render lock, so each step that replaces or removes files
// takes that lock too.
const char* writeRefusal() {
  if (activityManager.isForegroundHomeShown()) return nullptr;
  return std::strcmp(activityManager.foregroundName(), "Home") == 0 ? "busy" : "go to Home first";
}

bool isDirectory(const std::string& path) {
  HalFile file = Storage.open(path.c_str());
  return file && file.isDirectory();
}

bool isActiveFontFamily(const std::string& family) {
  return strcasecmp(family.c_str(), SETTINGS.sdFontFamilyName) == 0;
}

// Fills dst; false if the bytes stop for FRAME_GAP_MS.
bool readExact(HalSerialBulkRead& rx, uint8_t* dst, size_t size) {
  size_t got = 0;
  uint32_t lastByteAt = millis();
  while (got < size) {
    const size_t n = rx.read(dst + got, size - got);
    if (n > 0) {
      got += n;
      lastByteAt = millis();
    } else if (millis() - lastByteAt > FRAME_GAP_MS) {
      return false;
    } else {
      yield();
    }
  }
  return true;
}

void nak(uint32_t received, const char* reason) {
  LOG_DBG("CON", "PUT frame refused at %lu: %s", static_cast<unsigned long>(received), reason);
  sendLine("PUT_NAK %lu %s\n", static_cast<unsigned long>(received), reason);
}

// After a refused PUT the host may still have a frame on the way; swallow it so
// it does not reach the command parser as text.
void drainFrames(HalSerialBulkRead& rx, uint8_t* scratch) {
  uint32_t lastByteAt = millis();
  const uint32_t startedAt = lastByteAt;
  while (millis() - lastByteAt <= FRAME_GAP_MS && millis() - startedAt < HOST_TIMEOUT_MS) {
    if (rx.read(scratch, serialxfer::CHUNK_SIZE) > 0) {
      lastByteAt = millis();
    } else {
      yield();
    }
  }
}

// Receives `size` bytes as frames into `file`. Returns nullptr once all arrived and
// match `expectedCrc`, else why the transfer failed.
const char* receiveFrames(HalSerialBulkRead& rx, HalFile& file, uint8_t* buffer, uint32_t size, uint32_t expectedCrc,
                          PathKind kind, bool vectorFont) {
  sendLine("PUT_READY %u\n", static_cast<unsigned>(serialxfer::CHUNK_SIZE));
  uint32_t received = 0;
  uint32_t fileCrc = 0;
  uint32_t lastReplyAt = millis();
  uint32_t lastByteAt = lastReplyAt;
  bool partial = false;  // bytes arrived since the last reply
  while (received < size) {
    resetTaskWatchdogIfSubscribed();
    uint8_t c;
    if (rx.read(&c, 1) == 0) {
      const uint32_t now = millis();
      if (partial && now - lastByteAt > FRAME_GAP_MS) {
        nak(received, "lost");
        partial = false;
        lastReplyAt = now;
      } else if (!partial && now - lastReplyAt > HOST_TIMEOUT_MS) {
        return "timed out";
      }
      yield();
      continue;
    }
    lastByteAt = millis();
    partial = true;
    // Anything before a frame start is what is left of a damaged frame.
    if (c != serialxfer::FRAME_MAGIC[0]) continue;

    uint8_t header[serialxfer::FRAME_HEADER_SIZE];
    header[0] = c;
    serialxfer::FrameHeader frame;
    if (!readExact(rx, header + 1, sizeof(header) - 1) || !serialxfer::decodeFrameHeader(header, frame)) continue;
    if (frame.offset == serialxfer::CANCEL_OFFSET && frame.length == 0) return "cancelled";
    if (!readExact(rx, buffer, frame.length)) continue;

    uint32_t crc = esp_rom_crc32_le(0, serialxfer::crcCoveredHeader(header), serialxfer::CRC_COVERED_HEADER_SIZE);
    crc = esp_rom_crc32_le(crc, buffer, frame.length);
    if (const char* reason = serialxfer::checkFrame(frame, crc, received, size)) {
      nak(received, reason);
    } else {
      if (received == 0 && kind == PathKind::FontFile && !serialxfer::fontMagicOk(buffer, frame.length, vectorFont)) {
        return "not a font file";
      }
      if (file.write(buffer, frame.length) != frame.length) return "SD write failed (card full?)";
      fileCrc = esp_rom_crc32_le(fileCrc, buffer, frame.length);
      received += frame.length;
      sendLine("PUT_ACK %lu\n", static_cast<unsigned long>(received));
    }
    partial = false;
    lastReplyAt = millis();
  }
  if (kind == PathKind::FontFile && size < 8) return "not a font file";
  return fileCrc == expectedCrc ? nullptr : "file CRC does not match";
}

// Where a font lands: chosen like the web upload, so an installed family keeps its folder.
const char* resolveFontPath(const serialxfer::PathCheck& check, std::string& path, bool& vector, bool& newFamily) {
  const std::string family(check.family);
  const std::string fileName(check.fileName);
  if (!FontInstaller::isValidFamilyName(family.c_str())) return "family names use only A-Z, a-z, 0-9, - and _";
  vector = HalMemory::getPsramHeap().totalBytes > 0 && FontInstaller::isValidVectorFontFilename(fileName.c_str());
  if (!vector && !FontInstaller::isValidCpfontFilename(fileName.c_str())) {
    return "expects a .cpfont name of A-Z, a-z, 0-9, - and _";
  }
  char dest[160];
  // Room for the longer root, "/fonts" or "/.fonts"; buildFontPath would cut a longer path short.
  if (std::strlen(SdCardFontRegistry::FONTS_DIR_HIDDEN) + family.size() + fileName.size() + 2 >= sizeof(dest)) {
    return "font path too long";
  }
  FontInstaller::buildFontPath(family.c_str(), fileName.c_str(), dest, sizeof(dest));
  path = dest;
  // Loaded fonts reopen their file by path for glyphs, so one in use is never swapped.
  if (isActiveFontFamily(family) && Storage.exists(dest)) return "font in use; choose another reader font first";
  newFamily = SdCardFontRegistry::findFamilyRoot(family.c_str()) == nullptr;
  FontInstaller installer(sdFontSystem.registry());
  if (!installer.ensureFamilyDir(family.c_str())) return "could not create the font folder";
  return nullptr;
}

void putFile(const SerialCommand& cmd) {
  if (const char* why = writeRefusal()) return replyErr(cmd, why);
  const serialxfer::PathCheck check = serialxfer::checkPath(cmd.path, Access::Write);
  if (check.kind == PathKind::Invalid) return replyErr(cmd, check.error);

  std::string path(cmd.path);
  bool vectorFont = false;
  bool newFamily = false;
  if (check.kind == PathKind::FontFile) {
    if (const char* why = resolveFontPath(check, path, vectorFont, newFamily)) return replyErr(cmd, why);
  } else if (!isDirectory(path.substr(0, std::max<size_t>(path.rfind('/'), 1)))) {
    return replyErr(cmd, "no such folder");
  }
  if (Storage.exists(path.c_str())) {
    if (isDirectory(path)) return replyErr(cmd, "a folder has that name");
    if (!cmd.replace) return replyErr(cmd, "already exists (PUT REPLACE overwrites)");
  }

  // One chunk, held only for this transfer; the frame header stays on the stack.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(serialxfer::CHUNK_SIZE);
  if (!buffer) {
    LOG_ERR("CON", "OOM: %u byte transfer buffer", static_cast<unsigned>(serialxfer::CHUNK_SIZE));
    return replyErr(cmd, "out of memory");
  }
  // Staged like HttpDownloader: a failed transfer never leaves a partial file under the name.
  const std::string partPath = path + ".part";
  HalFile file;
  if (!Storage.openFileForWrite("CON", partPath, file)) return replyErr(cmd, "could not create the file");

  const unsigned long startedAt = millis();
  const char* error;
  {
    HalPowerManager::Lock fullSpeed;
    HalSerialBulkRead rx;
    error = receiveFrames(rx, file, buffer.get(), cmd.size, cmd.crc, check.kind, vectorFont);
    if (error) drainFrames(rx, buffer.get());
  }
  buffer.reset();
  if (!file.close() && !error) error = "SD write failed";
  if (!error) {
    RenderLock lock;
    if (Storage.replaceFile(partPath.c_str(), path.c_str())) {
      // The same follow-up as the web uploads.
      if (check.kind == PathKind::FontFile) {
        sdFontSystem.markRegistryDirty();
      } else {
        clearBookCache(path);
        if (library::isLibraryBookFile(path)) library::markLibraryIndexDirty();
      }
    } else {
      error = "could not rename the file";
    }
  }
  if (error) {
    Storage.remove(partPath.c_str());
    // Unlike the web upload, a failed first font of a family leaves no empty family behind.
    if (newFamily) Storage.rmdir(path.substr(0, path.rfind('/')).c_str());
    return replyErr(cmd, error);
  }
  LOG_INF("CON", "Received %s: %lu bytes in %lu ms", path.c_str(), static_cast<unsigned long>(cmd.size),
          millis() - startedAt);
  replyOk(cmd, path.c_str());
}

void getFile(const SerialCommand& cmd) {
  const serialxfer::PathCheck check = serialxfer::checkPath(cmd.path, Access::Read);
  if (check.kind == PathKind::Invalid) return replyErr(cmd, check.error);
  HalFile file;
  if (!Storage.openFileForRead("CON", std::string(cmd.path), file)) return replyErr(cmd, "no such file");
  if (file.isDirectory()) return replyErr(cmd, "is a folder");
  const uint32_t size = file.size();
  if (cmd.offset > size || !file.seekSet(cmd.offset)) return replyErr(cmd, "offset past the end");

  // A header and one chunk, so each frame goes out in one write and log lines
  // from other tasks fall between frames (ESP-IDF's own log output bypasses that
  // and can split one; the CRC catches it). Held only for this transfer.
  auto frame = makeUniqueNoThrow<uint8_t[]>(serialxfer::FRAME_HEADER_SIZE + serialxfer::CHUNK_SIZE);
  if (!frame) {
    LOG_ERR("CON", "OOM: %u byte transfer buffer",
            static_cast<unsigned>(serialxfer::FRAME_HEADER_SIZE + serialxfer::CHUNK_SIZE));
    return replyErr(cmd, "out of memory");
  }
  HalPowerManager::Lock fullSpeed;
  char detail[16];
  snprintf(detail, sizeof(detail), "%lu", static_cast<unsigned long>(size));
  replyOk(cmd, detail);

  uint8_t* const payload = frame.get() + serialxfer::FRAME_HEADER_SIZE;
  for (uint32_t offset = cmd.offset; offset < size;) {
    resetTaskWatchdogIfSubscribed();
    // A host that gives up sends its next command; a pulled cable makes HWCDC drop data unsent.
    if (logSerial.available() > 0 || !HalSerialBulkRead::cablePresent()) {
      sendLine("GET_ABORT stopped\n");
      return;
    }
    const auto length = static_cast<uint16_t>(std::min<uint32_t>(serialxfer::CHUNK_SIZE, size - offset));
    if (file.read(payload, length) != length) {
      sendLine("GET_ABORT read failed\n");
      return;
    }
    serialxfer::encodeFrameHeader({offset, length, 0}, frame.get());
    uint32_t crc = esp_rom_crc32_le(0, serialxfer::crcCoveredHeader(frame.get()), serialxfer::CRC_COVERED_HEADER_SIZE);
    serialxfer::encodeFrameHeader({offset, length, esp_rom_crc32_le(crc, payload, length)}, frame.get());
    const size_t frameSize = serialxfer::FRAME_HEADER_SIZE + length;
    if (logSerial.write(frame.get(), frameSize) != frameSize) {
      LOG_ERR("CON", "GET stopped at %lu: the host is not reading", static_cast<unsigned long>(offset));
      return;
    }
    offset += length;
  }
}

void listFolder(const SerialCommand& cmd) {
  const serialxfer::PathCheck check = serialxfer::checkPath(cmd.path, Access::Read);
  if (check.kind == PathKind::Invalid) return replyErr(cmd, check.error);
  HalFile folder = Storage.open(std::string(cmd.path).c_str());
  if (!folder) return replyErr(cmd, "no such folder");
  if (!folder.isDirectory()) return replyErr(cmd, "not a folder");
  auto name = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!name) {
    LOG_ERR("CON", "OOM: %u byte name buffer", static_cast<unsigned>(NAME_BUFFER_SIZE));
    return replyErr(cmd, "out of memory");
  }
  unsigned count = 0;
  for (HalFile entry = folder.openNextFile(); entry; entry = folder.openNextFile()) {
    resetTaskWatchdogIfSubscribed();
    if (!entry.getName(name.get(), NAME_BUFFER_SIZE)) continue;
    const bool dir = entry.isDirectory();
    sendLine("LS_ENTRY %c %lu %s\n", dir ? 'D' : 'F', dir ? 0UL : static_cast<unsigned long>(entry.size()), name.get());
    count++;
  }
  char detail[16];
  snprintf(detail, sizeof(detail), "%u", count);
  replyOk(cmd, detail);
}

void makeFolder(const SerialCommand& cmd) {
  if (const char* why = writeRefusal()) return replyErr(cmd, why);
  const serialxfer::PathCheck check = serialxfer::checkPath(cmd.path, Access::Write);
  if (check.kind == PathKind::Invalid) return replyErr(cmd, check.error);
  if (check.kind != PathKind::File) return replyErr(cmd, "font folders are made by PUT");
  const std::string path(cmd.path);
  if (Storage.exists(path.c_str()))
    return isDirectory(path) ? replyOk(cmd, "exists") : replyErr(cmd, "a file has that name");
  return Storage.mkdir(path.c_str()) ? replyOk(cmd) : replyErr(cmd, "could not create the folder");
}

void removeFontFamily(const SerialCommand& cmd, const std::string& family) {
  if (!FontInstaller::isValidFamilyName(family.c_str())) return replyErr(cmd, "no such font family");
  if (isActiveFontFamily(family)) return replyErr(cmd, "font in use; choose another reader font first");
  if (!SdCardFontRegistry::findFamilyRoot(family.c_str())) return replyErr(cmd, "no such font family");
  FontInstaller installer(sdFontSystem.registry());
  RenderLock lock;
  if (installer.deleteFamily(family.c_str()) != FontInstaller::Error::OK) return replyErr(cmd, "could not remove it");
  sdFontSystem.markRegistryDirty();
  replyOk(cmd);
}

void removeItem(const SerialCommand& cmd) {
  if (const char* why = writeRefusal()) return replyErr(cmd, why);
  const serialxfer::PathCheck check = serialxfer::checkPath(cmd.path, Access::Remove);
  if (check.kind == PathKind::Invalid) return replyErr(cmd, check.error);
  if (check.kind == PathKind::FontFamily) return removeFontFamily(cmd, std::string(check.family));
  if (check.kind == PathKind::FontFile && isActiveFontFamily(std::string(check.family))) {
    return replyErr(cmd, "font in use; choose another reader font first");
  }

  const std::string path(cmd.path);
  RenderLock lock;
  HalFile item = Storage.open(path.c_str());
  if (!item) return replyErr(cmd, "no such file");
  bool removed;
  if (item.isDirectory()) {
    // Like the web file manager: folders only once empty.
    if (HalFile entry = item.openNextFile()) return replyErr(cmd, "folder not empty");
    item.close();
    removed = Storage.rmdir(path.c_str());
  } else {
    item.close();
    removed = check.kind == PathKind::FontFile ? Storage.remove(path.c_str()) : removeBookFile(path);
  }
  if (check.kind == PathKind::FontFile) sdFontSystem.markRegistryDirty();
  return removed ? replyOk(cmd) : replyErr(cmd, "could not remove it");
}

}  // namespace

void runSerialFileCommand(const SerialCommand& cmd) {
  switch (cmd.type) {
    case SerialCommand::Type::List:
      return listFolder(cmd);
    case SerialCommand::Type::Get:
      return getFile(cmd);
    case SerialCommand::Type::Put:
      return putFile(cmd);
    case SerialCommand::Type::MakeDir:
      return makeFolder(cmd);
    case SerialCommand::Type::Remove:
      return removeItem(cmd);
    default:
      return;
  }
}

#endif  // ENABLE_SERIAL_FILES
