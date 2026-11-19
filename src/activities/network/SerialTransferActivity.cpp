#include "SerialTransferActivity.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HiddenItems.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstring>

#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/CrossPointWebServer.h"
#include "network/ProtectedPaths.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr uint8_t SYNC0 = 0xA5;
constexpr uint8_t SYNC1 = 0x5A;
constexpr uint32_t DEFAULT_BAUD = 115200;
// After a speed change the reader waits this long for a valid frame at the new speed
// and falls back to DEFAULT_BAUD otherwise, so the host can always recover the link.
constexpr unsigned long BAUD_CONFIRM_MS = 4000;

enum Cmd : uint8_t {
  CMD_HELLO = 0x01,
  CMD_LIST = 0x02,
  CMD_READ_OPEN = 0x03,
  CMD_READ = 0x04,
  CMD_CLOSE = 0x05,
  CMD_WRITE_OPEN = 0x06,
  CMD_WRITE = 0x07,
  CMD_WRITE_END = 0x08,
  CMD_MKDIR = 0x09,
  CMD_RENAME = 0x0A,
  CMD_DELETE = 0x0B,
  CMD_BAUD = 0x0C,
  CMD_STATUS = 0x0D,
  CMD_API = 0x0E,
  CMD_API_READ = 0x0F,
};

enum Status : uint8_t {
  ST_OK = 0,
  ST_BAD_CMD = 1,
  ST_BAD_ARG = 2,
  ST_NOT_FOUND = 3,
  ST_EXISTS = 4,
  ST_PROTECTED = 5,
  ST_IO = 6,
  ST_STATE = 7,
};

uint32_t crc32(const uint8_t* a, size_t an, const uint8_t* b = nullptr, size_t bn = 0, const uint8_t* c = nullptr,
               size_t cn = 0) {
  uint32_t crc = esp_rom_crc32_le(0, a, an);
  if (b && bn) crc = esp_rom_crc32_le(crc, b, bn);
  if (c && cn) crc = esp_rom_crc32_le(crc, c, cn);
  return crc;
}

uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
void wr32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

// A request path must be absolute, free of "." / ".." segments and control
// characters, and must not reach a credential store or a protected folder.
bool pathAllowed(const std::string& path) {
  if (path.empty() || path[0] != '/' || path.size() > 400) return false;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') i++;
    size_t j = i;
    while (j < path.size() && path[j] != '/') {
      if (static_cast<uint8_t>(path[j]) < 0x20) return false;
      j++;
    }
    const std::string seg = path.substr(i, j - i);
    if (seg == "." || seg == "..") return false;
    for (const auto* hidden : FsHelpers::HIDDEN_ITEMS) {
      if (seg == hidden) return false;
    }
    i = j;
  }
  return !protectedpaths::isSensitivePath(path.c_str());
}

// Print sink that appends to a std::string (API responses).
class StringSink final : public Print {
 public:
  explicit StringSink(std::string& out) : out_(out) {}
  size_t write(uint8_t c) override {
    out_.push_back(static_cast<char>(c));
    return 1;
  }
  size_t write(const uint8_t* data, size_t len) override {
    out_.append(reinterpret_cast<const char*>(data), len);
    return len;
  }

 private:
  std::string& out_;
};

std::string toPath(const uint8_t* p, size_t n) { return std::string(reinterpret_cast<const char*>(p), n); }

std::string trimSlash(std::string p) {
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

// The USB-C bridge is wired to UART0. With CDC-on-boot `Serial` is the native USB
// port, and UART0 is `Serial0`.
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
HardwareSerial& port() { return Serial0; }
#else
HardwareSerial& port() { return Serial; }
#endif
}  // namespace

void SerialTransferActivity::onEnter() {
  Activity::onEnter();
  rxBuf = static_cast<uint8_t*>(heap_caps_malloc(MAX_PAYLOAD, MALLOC_CAP_8BIT));
  txBuf = static_cast<uint8_t*>(heap_caps_malloc(MAX_PAYLOAD + 16, MALLOC_CAP_8BIT));
  if (!rxBuf || !txBuf) {
    setAction("Out of memory");
    exitRequested = true;
    requestUpdate();
    return;
  }
  // The log output shares the UART; mute it so only protocol frames flow.
  logSetMuted(true);
  port().end();
  port().setRxBufferSize(2 * MAX_PAYLOAD);
  port().begin(DEFAULT_BAUD);
  while (port().available() > 0) port().read();
  requestUpdate();
}

void SerialTransferActivity::onExit() {
  closeFiles();
  apiOut = std::string();
  if (writePart.empty() == false) Storage.remove(writePart.c_str());
  if (rxBuf) heap_caps_free(rxBuf);
  if (txBuf) heap_caps_free(txBuf);
  rxBuf = txBuf = nullptr;
  port().flush();
  port().end();
  port().begin(DEFAULT_BAUD);
  logSetMuted(false);
  Activity::onExit();
}

// The render task reads lastAction under RenderLock, so every write takes it too.
void SerialTransferActivity::setAction(std::string text) {
  RenderLock lock;
  lastAction = std::move(text);
}

void SerialTransferActivity::closeFiles() {
  if (readFile) readFile.close();
  if (writeFile) {
    writeFile.close();
    if (!writePart.empty()) Storage.remove(writePart.c_str());
  }
  writePart.clear();
  writeTarget.clear();
}

void SerialTransferActivity::loop() {
  if (!rxBuf || exitRequested) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || exitRequested) activityManager.goHome();
    return;
  }
  pump();
  if (baudConfirmDeadline != 0 && millis() > baudConfirmDeadline) {
    // Nothing valid arrived at the new speed: go back to the speed every host starts with.
    baudConfirmDeadline = 0;
    port().updateBaudRate(DEFAULT_BAUD);
    while (port().available() > 0) port().read();
    parse = Parse::Sync0;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Power) || mappedInput.wasHomeGesture()) {
    activityManager.goHome();
    return;
  }
  // Repaint the e-ink only between transfers and not more than every 2 s.
  if (dirty && millis() - lastRenderAt > 2000) {
    dirty = false;
    lastRenderAt = millis();
    requestUpdate();
  }
}

void SerialTransferActivity::pump() {
  const unsigned long burstStart = millis();
  // Handle up to ~80 ms of traffic per loop so the UI stays responsive.
  while (millis() - burstStart < 80) {
    if (parse != Parse::Sync0 && millis() - frameStartedAt > FRAME_TIMEOUT_MS) parse = Parse::Sync0;
    const int avail = port().available();
    if (avail <= 0) {
      if (parse == Parse::Sync0) return;
      delay(1);
      continue;
    }
    switch (parse) {
      case Parse::Sync0:
        if (port().read() == SYNC0) {
          parse = Parse::Sync1;
          frameStartedAt = millis();
        }
        break;
      case Parse::Sync1: {
        const int b = port().read();
        if (b == SYNC1) {
          parse = Parse::Header;
          headerGot = 0;
        } else if (b != SYNC0) {
          parse = Parse::Sync0;
        }
        break;
      }
      case Parse::Header:
        header[headerGot++] = static_cast<uint8_t>(port().read());
        if (headerGot == 4) {
          cmd = header[0];
          reqId = header[1];
          rxLen = static_cast<size_t>(header[2]) | (static_cast<size_t>(header[3]) << 8);
          rxGot = 0;
          crcGot = 0;
          parse = rxLen > MAX_PAYLOAD ? Parse::Sync0 : (rxLen == 0 ? Parse::Crc : Parse::Payload);
        }
        break;
      case Parse::Payload: {
        const size_t want = std::min(static_cast<size_t>(avail), rxLen - rxGot);
        const size_t got = port().readBytes(rxBuf + rxGot, want);
        rxGot += got;
        if (rxGot == rxLen) parse = Parse::Crc;
        break;
      }
      case Parse::Crc:
        crcBytes[crcGot++] = static_cast<uint8_t>(port().read());
        if (crcGot == 4) {
          parse = Parse::Sync0;
          if (rd32(crcBytes) == crc32(header, 4, rxBuf, rxLen)) {
            handleFrame();
          }  // a corrupt frame gets no reply; the host times out and retries
        }
        break;
    }
  }
}

void SerialTransferActivity::reply(uint8_t status, const uint8_t* payload, size_t len) {
  if (payload && payload != txBuf + REPLY_HEAD) memcpy(txBuf + REPLY_HEAD, payload, len);
  txBuf[0] = SYNC0;
  txBuf[1] = SYNC1;
  txBuf[2] = status;
  txBuf[3] = reqId;
  txBuf[4] = static_cast<uint8_t>(len);
  txBuf[5] = static_cast<uint8_t>(len >> 8);
  // Layout: [0..1] sync, [2] status, [3] request id, [4..5] length, [6..] payload, then CRC32.
  const uint32_t crc = crc32(txBuf + 2, 4, txBuf + REPLY_HEAD, len);
  wr32(txBuf + REPLY_HEAD + len, crc);
  port().write(txBuf, REPLY_HEAD + len + 4);
  port().flush();
}

void SerialTransferActivity::handleFrame() {
  baudConfirmDeadline = 0;  // a valid frame arrived at the current speed
  txLen = 0;
  uint8_t status = ST_BAD_CMD;
  const uint8_t* p = rxBuf;
  const size_t n = rxLen;

  switch (cmd) {
    case CMD_HELLO: {
      static const char hello[] = "CPXFER1";
      memcpy(txBuf + REPLY_HEAD, hello, sizeof(hello));
      txLen = sizeof(hello);
      if (!hostSeen) {
        hostSeen = true;
        setAction("Computer connected");
        dirty = true;
      }
      status = ST_OK;
      break;
    }
    case CMD_STATUS: {
      const int len = snprintf(reinterpret_cast<char*>(txBuf + REPLY_HEAD), 200,
                               "{\"version\":\"%s\",\"freeHeap\":%u,\"uptime\":%lu,\"device\":\"Sticky\"}",
                               CROSSPOINT_VERSION, static_cast<unsigned>(ESP.getFreeHeap()), millis() / 1000UL);
      txLen = len > 0 ? static_cast<size_t>(len) : 0;
      status = ST_OK;
      break;
    }
    case CMD_LIST:
      status = cmdList(p, n);
      break;
    case CMD_READ_OPEN:
      status = cmdReadOpen(p, n);
      break;
    case CMD_READ:
      status = cmdRead(p, n);
      break;
    case CMD_CLOSE:
      // Also abandons an unfinished upload: the host sends CLOSE when it gives up on one.
      if (readFile) readFile.close();
      if (writeFile) closeFiles();
      status = ST_OK;
      break;
    case CMD_WRITE_OPEN:
      status = cmdWriteOpen(p, n);
      break;
    case CMD_WRITE:
      status = cmdWrite(p, n);
      break;
    case CMD_WRITE_END:
      if (haveEndResult && reqId == endReqId) {
        status = endStatus;  // retry of a commit whose reply was lost: report the original result
      } else {
        status = cmdWriteEnd();
        haveEndResult = true;
        endReqId = reqId;
        endStatus = status;
      }
      break;
    case CMD_MKDIR:
      status = cmdMkdir(p, n);
      break;
    case CMD_RENAME:
      status = cmdRename(p, n);
      break;
    case CMD_DELETE:
      status = cmdDelete(p, n);
      break;
    case CMD_API:
      status = cmdApi(p, n);
      break;
    case CMD_API_READ:
      status = cmdApiRead(p, n);
      break;
    case CMD_BAUD:
      if (n == 4) {
        pendingBaud = rd32(p);
        status = (pendingBaud >= 9600 && pendingBaud <= 3000000) ? ST_OK : ST_BAD_ARG;
      } else {
        status = ST_BAD_ARG;
      }
      break;
    default:
      break;
  }

  reply(status, nullptr, txLen);
  if (cmd == CMD_BAUD && status == ST_OK && pendingBaud != 0) {
    port().updateBaudRate(pendingBaud);
    baudConfirmDeadline = pendingBaud == DEFAULT_BAUD ? 0 : millis() + BAUD_CONFIRM_MS;
    pendingBaud = 0;
  }
}

// LIST: req = u32 startIndex + path. resp = u8 more + entries
// (u8 flags[bit0=dir], u32 size, u8 nameLen, name).
uint8_t SerialTransferActivity::cmdList(const uint8_t* p, size_t n) {
  if (n < 5) return ST_BAD_ARG;
  const uint32_t start = rd32(p);
  const std::string path = trimSlash(toPath(p + 4, n - 4));
  if (!pathAllowed(path)) return ST_PROTECTED;
  HalFile dir = Storage.open(path.c_str());
  if (!dir) return ST_NOT_FOUND;
  if (!dir.isDirectory()) {
    dir.close();
    return ST_BAD_ARG;
  }
  uint8_t* out = txBuf + REPLY_HEAD;
  size_t used = 1;  // out[0] = more flag
  out[0] = 0;
  uint32_t index = 0;
  char name[256];
  HalFile f = dir.openNextFile();
  while (f) {
    const size_t nameLen = f.getName(name, sizeof(name));
    bool hide = nameLen == 0 || nameLen > 255 || (!SETTINGS.showHiddenFiles && name[0] == '.');
    for (const auto* hidden : FsHelpers::HIDDEN_ITEMS) {
      if (strcmp(name, hidden) == 0) hide = true;
    }
    if (!hide) {
      if (index >= start) {
        if (used + 6 + nameLen > MAX_PAYLOAD - 16) {
          out[0] = 1;  // more entries remain
          f.close();
          break;
        }
        const bool isDir = f.isDirectory();
        out[used++] = isDir ? 1 : 0;
        wr32(out + used, isDir ? 0 : static_cast<uint32_t>(f.size()));
        used += 4;
        out[used++] = static_cast<uint8_t>(nameLen);
        memcpy(out + used, name, nameLen);
        used += nameLen;
      }
      index++;
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  txLen = used;
  return ST_OK;
}

uint8_t SerialTransferActivity::cmdReadOpen(const uint8_t* p, size_t n) {
  const std::string path = trimSlash(toPath(p, n));
  if (!pathAllowed(path)) return ST_PROTECTED;
  if (readFile) readFile.close();
  if (!Storage.openFileForRead("SER", path.c_str(), readFile)) return ST_NOT_FOUND;
  if (readFile.isDirectory()) {
    readFile.close();
    return ST_BAD_ARG;
  }
  wr32(txBuf + REPLY_HEAD, static_cast<uint32_t>(readFile.size()));
  txLen = 4;
  setAction("Sending " + path.substr(path.rfind('/') + 1));
  dirty = true;
  return ST_OK;
}

// READ: u32 offset + u16 length.
uint8_t SerialTransferActivity::cmdRead(const uint8_t* p, size_t n) {
  if (n != 6 || !readFile) return n != 6 ? ST_BAD_ARG : ST_STATE;
  const uint32_t off = rd32(p);
  size_t len = static_cast<size_t>(p[4]) | (static_cast<size_t>(p[5]) << 8);
  if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;
  if (readFile.position() != off && !readFile.seek(off)) return ST_IO;
  const int got = readFile.read(txBuf + REPLY_HEAD, len);
  if (got < 0) return ST_IO;
  txLen = static_cast<size_t>(got);
  return ST_OK;
}

// WRITE_OPEN: u8 overwrite + path. Data goes to "<path>.part" until WRITE_END.
uint8_t SerialTransferActivity::cmdWriteOpen(const uint8_t* p, size_t n) {
  if (n < 2) return ST_BAD_ARG;
  bool overwrite = p[0] != 0;
  std::string path = trimSlash(toPath(p + 1, n - 1));
  closeFiles();
  writeIsFont = false;
  writeIsVector = false;
  if (path.rfind("/@font/", 0) == 0) {
    // Font upload: "/@font/<family>/<file>" is mapped to the font folder, with the
    // same validation as the web uploader.
    const std::string rest = path.substr(7);
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) return ST_BAD_ARG;
    const std::string family = rest.substr(0, slash);
    const std::string file = rest.substr(slash + 1);
    const bool psram = HalMemory::getPsramHeap().totalBytes > 0;
    writeIsVector = psram && FontInstaller::isValidVectorFontFilename(file.c_str());
    if (!FontInstaller::isValidFamilyName(family.c_str()) ||
        (!writeIsVector && !FontInstaller::isValidCpfontFilename(file.c_str()))) {
      return ST_BAD_ARG;
    }
    FontInstaller installer(sdFontSystem.registry());
    if (!installer.ensureFamilyDir(family.c_str())) return ST_IO;
    char real[128];
    FontInstaller::buildFontPath(family.c_str(), file.c_str(), real, sizeof(real));
    path = real;
    overwrite = true;
    writeIsFont = true;
  } else if (!pathAllowed(path) || path == "/") {
    return ST_PROTECTED;
  }
  if (Storage.exists(path.c_str())) {
    HalFile existing = Storage.open(path.c_str());
    const bool isDir = existing && existing.isDirectory();
    if (existing) existing.close();
    if (isDir) return ST_EXISTS;
    if (!overwrite) return ST_EXISTS;
  }
  // Stage next to the target under a name nobody owns: never delete or reuse an existing file.
  std::string part;
  for (int i = 0; i < 100; i++) {
    part = path + (i == 0 ? ".part" : ".part" + std::to_string(i));
    if (!Storage.exists(part.c_str())) break;
    part.clear();
  }
  if (part.empty()) return ST_IO;
  haveEndResult = false;
  const size_t slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0) Storage.mkdir(path.substr(0, slash).c_str());
  if (!Storage.openFileForWrite("SER", part.c_str(), writeFile)) return ST_IO;
  writeTarget = path;
  writePart = part;
  writeOverwrite = overwrite;
  setAction("Receiving " + path.substr(path.rfind('/') + 1));
  dirty = true;
  return ST_OK;
}

// WRITE: u32 offset (must equal the current length) + data.
uint8_t SerialTransferActivity::cmdWrite(const uint8_t* p, size_t n) {
  if (n < 4 || !writeFile) return n < 4 ? ST_BAD_ARG : ST_STATE;
  const size_t len = n - 4;
  const uint32_t off = rd32(p);
  const uint32_t pos = static_cast<uint32_t>(writeFile.position());
  if (off + len == pos) return ST_OK;  // retried frame whose reply was lost: already written
  if (off != pos) return ST_STATE;
  if (len == 0) return ST_OK;
  return writeFile.write(p + 4, len) == len ? ST_OK : ST_IO;
}

uint8_t SerialTransferActivity::cmdWriteEnd() {
  if (!writeFile) return ST_STATE;
  writeFile.flush();
  writeFile.close();
  uint8_t status = ST_OK;
  if (writeIsFont) {
    // Same magic-byte check as the web uploader.
    uint8_t magic[8] = {0};
    HalFile part;
    bool ok = false;
    if (Storage.openFileForRead("SER", writePart.c_str(), part)) {
      ok = part.read(magic, 8) == 8;
      part.close();
    }
    if (ok) {
      if (writeIsVector) {
        static const uint8_t ttf[4] = {0x00, 0x01, 0x00, 0x00};
        ok = memcmp(magic, ttf, 4) == 0 || memcmp(magic, "OTTO", 4) == 0 || memcmp(magic, "true", 4) == 0;
      } else {
        ok = memcmp(magic, "CPFONT\0\0", 8) == 0;
      }
    }
    if (!ok) {
      Storage.remove(writePart.c_str());
      writePart.clear();
      writeTarget.clear();
      return ST_BAD_ARG;
    }
  }
  // Keep the original aside until the new file is in place, so a failed rename loses nothing.
  std::string backup;
  bool replaced = false;
  if (Storage.exists(writeTarget.c_str())) {
    if (!writeOverwrite) {
      Storage.remove(writePart.c_str());
      status = ST_EXISTS;
    }
    for (int i = 0; status == ST_OK && i < 100; i++) {
      backup = writeTarget + (i == 0 ? ".bak" : ".bak" + std::to_string(i));
      if (!Storage.exists(backup.c_str())) break;
      backup.clear();
    }
    if (status != ST_OK) {
      // the target appeared during the upload and overwriting was not requested
    } else if (backup.empty() || !Storage.rename(writeTarget.c_str(), backup.c_str())) {
      Storage.remove(writePart.c_str());
      status = ST_IO;
      backup.clear();
    } else {
      replaced = true;
    }
  }
  if (status == ST_OK) {
    if (Storage.rename(writePart.c_str(), writeTarget.c_str())) {
      if (replaced) {
        Storage.remove(backup.c_str());
        if (!writeIsFont) {
          clearBookCache(writeTarget);
          ClippingStore::deleteForFilePath(writeTarget, "epub");
        }
      }
    } else {
      if (replaced) Storage.rename(backup.c_str(), writeTarget.c_str());  // put the original back
      Storage.remove(writePart.c_str());
      status = ST_IO;
    }
  }
  writePart.clear();
  writeTarget.clear();
  if (writeIsFont && status == ST_OK) sdFontSystem.markRegistryDirty();
  writeIsFont = false;
  if (status == ST_OK) filesDone++;
  setAction(status == ST_OK ? "Received " + std::to_string(filesDone) + " file(s)" : std::string("Upload failed"));
  dirty = true;
  return status;
}

// API: u8 method (0 GET, 1 POST) + uri [+ NUL + body]. Runs the same code as the web
// server's /api/settings and /api/fonts handlers. Reply: u16 HTTP status + u32 body
// length; the body is then fetched with API_READ (u32 offset + u16 length).
uint8_t SerialTransferActivity::cmdApi(const uint8_t* p, size_t n) {
  if (n < 2) return ST_BAD_ARG;
  const bool post = p[0] == 1;
  const uint8_t* nul = static_cast<const uint8_t*>(memchr(p + 1, 0, n - 1));
  const std::string uri = toPath(p + 1, nul ? static_cast<size_t>(nul - (p + 1)) : n - 1);
  const std::string body = nul ? toPath(nul + 1, n - static_cast<size_t>(nul - p) - 1) : std::string();
  std::string route = uri;
  std::string query;
  const size_t q = uri.find('?');
  if (q != std::string::npos) {
    route = uri.substr(0, q);
    query = uri.substr(q + 1);
  }

  apiOut.clear();
  uint16_t http = 404;
  StringSink sink(apiOut);
  if (!post && route == "/api/settings") {
    String keys;
    if (query.rfind("keys=", 0) == 0) keys = query.substr(5).c_str();
    CrossPointWebServer::writeSettingsJson(sink, keys);
    http = 200;
  } else if (post && route == "/api/settings") {
    String error;
    const int applied = CrossPointWebServer::applySettingsJson(String(body.c_str()), error);
    if (applied < 0) {
      apiOut = error.c_str();
      http = 400;
    } else {
      apiOut = "Applied " + std::to_string(applied) + " setting(s)";
      http = 200;
    }
  } else if (!post && route == "/api/fonts") {
    sdFontSystem.refreshIfDirty();
    http = CrossPointWebServer::writeFontListJson(sink) ? 200 : 500;
  } else if (post && route == "/api/fonts/delete") {
    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc["family"].is<const char*>()) {
      apiOut = "{\"error\":\"Invalid request\"}";
      http = 400;
    } else if (CrossPointWebServer::deleteFontFamily(doc["family"])) {
      apiOut = "{\"ok\":true}";
      http = 200;
    } else {
      apiOut = "{\"error\":\"Delete failed\"}";
      http = 500;
    }
  }
  txBuf[REPLY_HEAD] = static_cast<uint8_t>(http);
  txBuf[REPLY_HEAD + 1] = static_cast<uint8_t>(http >> 8);
  wr32(txBuf + REPLY_HEAD + 2, static_cast<uint32_t>(apiOut.size()));
  txLen = 6;
  setAction("Settings / fonts");
  dirty = true;
  return ST_OK;
}

uint8_t SerialTransferActivity::cmdApiRead(const uint8_t* p, size_t n) {
  if (n != 6) return ST_BAD_ARG;
  const uint32_t off = rd32(p);
  size_t len = static_cast<size_t>(p[4]) | (static_cast<size_t>(p[5]) << 8);
  if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;
  if (off > apiOut.size()) return ST_BAD_ARG;
  len = std::min(len, static_cast<size_t>(apiOut.size() - off));
  memcpy(txBuf + REPLY_HEAD, apiOut.data() + off, len);
  txLen = len;
  return ST_OK;
}

uint8_t SerialTransferActivity::cmdMkdir(const uint8_t* p, size_t n) {
  const std::string path = trimSlash(toPath(p, n));
  if (!pathAllowed(path) || path == "/") return ST_PROTECTED;
  if (Storage.exists(path.c_str())) return ST_EXISTS;
  return Storage.mkdir(path.c_str()) ? ST_OK : ST_IO;
}

// RENAME: oldPath NUL newPath. Used for rename and move (files only).
uint8_t SerialTransferActivity::cmdRename(const uint8_t* p, size_t n) {
  const uint8_t* nul = static_cast<const uint8_t*>(memchr(p, 0, n));
  if (!nul) return ST_BAD_ARG;
  const std::string from = trimSlash(toPath(p, static_cast<size_t>(nul - p)));
  const std::string to = trimSlash(toPath(nul + 1, n - static_cast<size_t>(nul - p) - 1));
  if (!pathAllowed(from) || !pathAllowed(to) || from == "/" || to == "/") return ST_PROTECTED;
  if (!Storage.exists(from.c_str())) return ST_NOT_FOUND;
  if (Storage.exists(to.c_str())) return ST_EXISTS;
  HalFile f = Storage.open(from.c_str());
  const bool isDir = f && f.isDirectory();
  if (f) f.close();
  if (isDir) return ST_BAD_ARG;  // like the web file manager, files only
  return Storage.rename(from.c_str(), to.c_str()) ? ST_OK : ST_IO;
}

uint8_t SerialTransferActivity::cmdDelete(const uint8_t* p, size_t n) {
  const std::string path = trimSlash(toPath(p, n));
  if (!pathAllowed(path) || path == "/") return ST_PROTECTED;
  if (!Storage.exists(path.c_str())) return ST_NOT_FOUND;
  HalFile f = Storage.open(path.c_str());
  if (f && f.isDirectory()) {
    HalFile entry = f.openNextFile();
    const bool notEmpty = static_cast<bool>(entry);
    if (entry) entry.close();
    f.close();
    if (notEmpty) return ST_EXISTS;  // folder not empty
    return Storage.rmdir(path.c_str()) ? ST_OK : ST_IO;
  }
  if (f) f.close();
  return removeBookFile(path.c_str()) ? ST_OK : ST_IO;
}

void SerialTransferActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Serial Transfer");
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  int y = (pageHeight - lineH * 5) / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, y, lastAction.c_str(), true, EpdFontFamily::BOLD);
  y += lineH * 2;
  if (!hostSeen) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, "Keep the USB-C cable plugged in and");
    renderer.drawCenteredText(UI_10_FONT_ID, y + lineH, "open crosspoint-serial.html on the computer.");
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, y, "Use crosspoint-serial.html on the computer.");
  }
  renderer.drawCenteredText(SMALL_FONT_ID, y + lineH * 3, "Press Back to leave this mode.");
  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
