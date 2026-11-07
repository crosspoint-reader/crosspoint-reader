#include "LocalSendReceiver.h"
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_random.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <unistd.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>

#include "LocalSendJson.h"
#include "util/TaskWatchdog.h"

namespace {
constexpr uint16_t PORT = 53317;
constexpr const char* GROUP = "224.0.0.167";
void randomId(char out[33]) {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    out[i * 2] = HEX_DIGITS[bytes[i] >> 4];
    out[i * 2 + 1] = HEX_DIGITS[bytes[i] & 15];
  }
  out[32] = 0;
}
void hexDigest(const uint8_t* bytes, char out[65]) {
  constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) {
    out[i * 2] = HEX_DIGITS[bytes[i] >> 4];
    out[i * 2 + 1] = HEX_DIGITS[bytes[i] & 15];
  }
  out[64] = 0;
}
void deviceInfo(JsonDocument& doc, const char* fingerprint, const char* alias) {
  doc["alias"] = alias;
  doc["version"] = "2.0";
  doc["deviceModel"] = "CrossPoint reader";
  doc["deviceType"] = "headless";
  doc["fingerprint"] = fingerprint;
  doc["port"] = PORT;
  doc["protocol"] = "http";
  doc["download"] = false;
}
// Reclaim only uploads staged by this receiver (32 lowercase hex characters + ".part").
void reclaimStagingFiles() {
  constexpr const char* DIR = "/.crosspoint/localsend";
  auto directory = Storage.open(DIR);
  if (!directory || !directory.isDirectory()) return;
  while (auto entry = directory.openNextFile()) {
    char name[48]{};
    entry.getName(name, sizeof(name));
    name[sizeof(name) - 1] = '\0';
    bool staging = !entry.isDirectory() && std::strlen(name) == 37 && !std::strcmp(name + 32, ".part");
    for (size_t i = 0; staging && i < 32; ++i)
      if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) staging = false;
    entry.close();
    if (!staging) continue;
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", DIR, name);
    if (!Storage.remove(path)) LOG_ERR("LSEND", "Could not reclaim abandoned upload %s", path);
  }
  directory.close();
}
}  // namespace
bool LocalSendReceiver::validDeviceId(const char* id) {
  if (!id || !*id || std::strlen(id) > 32) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(id); *p; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' ||
          *p == ' '))
      return false;
  }
  return id[0] != ' ' && id[std::strlen(id) - 1] != ' ';
}
bool LocalSendReceiver::setAdvertisedId(const char* id) {
  if (!validDeviceId(id)) return false;
  std::snprintf(advertisedAlias, sizeof(advertisedAlias), "CrossPoint(%s)", id);
  return true;
}
bool LocalSendReceiver::validFingerprint(const char* id) {
  if (!id || std::strlen(id) != 32) return false;
  for (const char* p = id; *p; ++p)
    if ((*p < '0' || *p > '9') && (*p < 'a' || *p > 'f')) return false;
  return true;
}
bool LocalSendReceiver::setFingerprint(const char* id) {
  if (!validFingerprint(id)) return false;
  std::memcpy(fingerprint, id, sizeof(fingerprint));
  return true;
}
LocalSendReceiver::LocalSendReceiver() { randomId(fingerprint); }
LocalSendReceiver::~LocalSendReceiver() { stop(); }
bool LocalSendReceiver::setDestination(const char* path) {
  if (!localsend::safeDestinationFolder(path)) return false;
  std::memcpy(destination, path, std::strlen(path) + 1);
  return true;
}
bool LocalSendReceiver::begin(CancelCheck check, void* context) {
  stop();
  if (WiFi.status() != WL_CONNECTED) return false;
  reclaimStagingFiles();
  cancelCheck = check;
  cancelContext = context;
  listener.begin();
  if (!listener) {
    LOG_ERR("LSEND", "Cannot listen on port %u", PORT);
    return false;
  }
  const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd >= 0) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    ip_mreq membership{};
    membership.imr_multiaddr.s_addr = inet_addr(GROUP);
    membership.imr_interface.s_addr = uint32_t(WiFi.localIP());
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) == 0 &&
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &membership.imr_interface, sizeof(membership.imr_interface)) == 0 &&
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) == 0)
      discoverySocket = fd;
    else
      close(fd);
  }
  if (discoverySocket < 0) LOG_ERR("LSEND", "Multicast unavailable; use direct IP");
  lastReply = millis() - 250;
  announce(true);
  LOG_INF("LSEND", "HTTP receiver ready at %s:%u; files go to %s", WiFi.localIP().toString().c_str(), PORT,
          destination);
  return true;
}
void LocalSendReceiver::stop() {
  pendingClient.stop();
  pendingClient = NetworkClient{};
  listener.end();
  if (discoverySocket >= 0) close(discoverySocket);
  discoverySocket = -1;
  session.reset();
  savedName[0] = 0;
  savedBytes = 0;
  savedSequence = 0;
}
bool LocalSendReceiver::cancelled() const { return cancelCheck && cancelCheck(cancelContext); }
bool LocalSendReceiver::receive(NetworkClient& client, uint8_t* bytes, size_t length, uint32_t started,
                                uint32_t totalTimeout, uint32_t idleTimeout) {
  size_t got = 0;
  uint32_t progress = millis();
  while (got < length) {
    if (cancelled() || millis() - started >= totalTimeout || millis() - progress >= idleTimeout) return false;
    const int available = client.available();
    if (available > 0) {
      const size_t take = std::min(length - got, size_t(available));
      const int n = client.read(bytes + got, take);
      if (n <= 0 || size_t(n) > take) return false;
      got += size_t(n);
      progress = millis();
    } else if (!client.connected())
      return false;
    resetTaskWatchdogIfSubscribed();
    delay(1);
  }
  return true;
}
int LocalSendReceiver::receiveLine(NetworkClient& client, char* line, size_t capacity, uint32_t started) {
  for (size_t used = 0; used + 1 < capacity; ++used) {
    uint8_t byte;
    if (!receive(client, &byte, 1, started, 600000)) return 408;
    if (byte == '\r') {
      if (!receive(client, &byte, 1, started, 600000)) return 408;
      if (byte != '\n') return 400;
      line[used] = 0;
      return 200;
    }
    if (byte < 32 || byte == 127) return 400;
    line[used] = char(byte);
  }
  return 400;
}
void LocalSendReceiver::reply(NetworkClient& client, int code, const char* body) {
  if (code >= 400 && !*body) body = "{\"message\":\"LocalSend request rejected; see LSEND serial log\"}";
  if (code >= 400) LOG_ERR("LSEND", "HTTP %d", code);
  char header[160];
  const int n = std::snprintf(
      header, sizeof(header),
      "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", code,
      code == 200 ? "OK" : "Error", unsigned(std::strlen(body)));
  if (n <= 0 || size_t(n) >= sizeof(header)) return;
  if (client.write(reinterpret_cast<const uint8_t*>(header), size_t(n)) != size_t(n)) {
    LOG_ERR("LSEND", "Could not write HTTP %d response header", code);
    return;
  }
  if (client.write(reinterpret_cast<const uint8_t*>(body), std::strlen(body)) != std::strlen(body))
    LOG_ERR("LSEND", "Could not write HTTP %d response body", code);
}
void LocalSendReceiver::sendInfo(NetworkClient& client) {
  localsend::BoundedJsonAllocator allocator;
  JsonDocument doc(&allocator);
  deviceInfo(doc, fingerprint, advertisedAlias);
  char body[384];
  if (measureJson(doc) >= sizeof(body)) {
    reply(client, 500);
    return;
  }
  serializeJson(doc, body, sizeof(body));
  reply(client, 200, body);
}
void LocalSendReceiver::serve(NetworkClient& client) {
  client.setTimeout(2);
  client.setNoDelay(true);
  const uint32_t started = millis();
  auto header = makeUniqueNoThrow<uint8_t[]>(2049);
  if (!header) {
    LOG_ERR("LSEND", "OOM: 2049-byte request header");
    reply(client, 500);
    return;
  }
  size_t length = 0;
  while (length < 2048) {
    if (!receive(client, header.get() + length, 1, started, 10000)) {
      reply(client, 408);
      return;
    }
    if (!header[length++]) {
      reply(client, 400);
      return;
    }
    if (length >= 4 && !std::memcmp(header.get() + length - 4, "\r\n\r\n", 4)) break;
  }
  if (length < 4 || std::memcmp(header.get() + length - 4, "\r\n\r\n", 4)) {
    reply(client, 431);
    return;
  }
  localsend::Request request;
  const int parsed = localsend::parseRequest(reinterpret_cast<char*>(header.get()), request);
  header.reset();
  if (parsed != 200) {
    LOG_ERR("LSEND", "Request framing rejected: code=%d route=%u chunked=%u length=%u", parsed, unsigned(request.route),
            unsigned(request.chunked), unsigned(request.length));
    reply(client, parsed);
    return;
  }
  const uint32_t peer = uint32_t(client.remoteIP());
  if (session.expired(millis())) session.reset();
  if (request.route == localsend::Route::Upload) {
    const int result = upload(client, request);
    LOG_INF("LSEND", "Upload completed: HTTP %d (chunked=%u)", result, unsigned(request.chunked));
    reply(client, result);
    return;
  }
  if (request.route == localsend::Route::Cancel && !session.cancellable(request.sessionId, peer, millis())) {
    reply(client, 403);
    return;
  }
  if (request.expectContinue) client.print("HTTP/1.1 100 Continue\r\n\r\n");
  auto body = request.length ? makeUniqueNoThrow<uint8_t[]>(request.length + 1) : nullptr;
  if (request.length && !body) {
    LOG_ERR("LSEND", "OOM: %u-byte metadata", unsigned(request.length + 1));
    reply(client, 500);
    return;
  }
  if (request.length && !receive(client, body.get(), request.length, started, 10000)) {
    reply(client, 408);
    return;
  }
  switch (request.route) {
    case localsend::Route::Info:
      sendInfo(client);
      return;
    case localsend::Route::Register: {
      localsend::BoundedJsonAllocator allocator;
      JsonDocument doc(&allocator);
      if (deserializeJson(doc, body.get(), request.length, DeserializationOption::NestingLimit(8)) ||
          !doc["fingerprint"].is<const char*>()) {
        reply(client, 400);
        return;
      }
      doc.clear();
      body.reset();
      sendInfo(client);
      return;
    }
    case localsend::Route::Cancel:
      session.reset();
      reply(client, 200);
      return;
    case localsend::Route::Prepare: {
      char id[33], token[33];
      randomId(id);
      randomId(token);
      const char* rejectionReason = nullptr;
      const int result = session.prepare(reinterpret_cast<const char*>(body.get()), request.length, peer, millis(), id,
                                         token, &rejectionReason);
      body.reset();
      if (result != 200) {
        const char* reason = rejectionReason ? rejectionReason : "Offer rejected";
        LOG_ERR("LSEND", "Prepare rejected: HTTP %d (%s); metadata=%u bytes", result, reason, unsigned(request.length));
        char response[160];
        std::snprintf(response, sizeof(response), "{\"message\":\"%s\"}", reason);
        reply(client, result, response);
        return;
      }
      for (uint8_t i = 0; i < session.fileCount; ++i) {
        char path[256];
        std::snprintf(path, sizeof(path), "%s%s%s", destination, std::strcmp(destination, "/") ? "/" : "",
                      session.files[i].fileName);
        if (Storage.exists(path)) {
          LOG_ERR("LSEND", "Prepare rejected: file already exists at %s (preserved)", path);
          session.reset();
          reply(client, 403, "{\"message\":\"File already exists in destination folder\"}");
          return;
        }
      }
      session.state = localsend::State::AwaitingConsent;
      pendingSince = millis();
      pendingClient = client;
      LOG_INF("LSEND", "Waiting for transfer acceptance");
      return;
    }
    case localsend::Route::Upload:
      return;  // Dispatched before metadata allocation.
  }
}
int LocalSendReceiver::upload(NetworkClient& client, const localsend::Request& request) {
  auto* offered =
      session.authorizedFile(request.sessionId, request.fileId, request.token, uint32_t(client.remoteIP()), millis());
  if (!offered) {
    LOG_ERR("LSEND", "Upload rejected: session expired, file already received, or peer/token mismatch");
    return 403;
  }
  if (!request.chunked && request.length != offered->expected) {
    LOG_ERR("LSEND", "Upload length %u differs from offer %u", unsigned(request.length), unsigned(offered->expected));
    return 400;
  }
  LOG_INF("LSEND", "Upload start: %s size=%u chunked=%u", offered->fileName, unsigned(offered->expected),
          unsigned(request.chunked));
  // A failed upload invalidates the remaining tokens; successful files already
  // published remain intact. A successful file leaves other offers prepared.
  bool succeeded = false;
  ScopedCleanup resetSession{[this, &succeeded] {
    if (!succeeded) session.reset();
  }};
  if ((std::strcmp(destination, "/") && !Storage.ensureDirectoryExists(destination)) ||
      !Storage.ensureDirectoryExists("/.crosspoint") || !Storage.ensureDirectoryExists("/.crosspoint/localsend"))
    return 500;
  char path[256], temporary[96];
  std::snprintf(path, sizeof(path), "%s%s%s", destination, std::strcmp(destination, "/") ? "/" : "", offered->fileName);
  std::snprintf(temporary, sizeof(temporary), "/.crosspoint/localsend/%s.part", offered->token);
  if (Storage.exists(path)) {
    LOG_ERR("LSEND", "Upload rejected: file already exists at %s (preserved)", path);
    return 409;
  }
  auto file = Storage.open(temporary, O_WRITE | O_CREAT | O_EXCL);
  if (!file) {
    LOG_ERR("LSEND", "Cannot create staging file");
    return 500;
  }
  bool published = false;
  ScopedCleanup cleanup{[&] {
    if (file) file.close();
    if (!published && !Storage.remove(temporary)) LOG_ERR("LSEND", "Could not remove %s", temporary);
  }};
  wc_Sha256 hash;
  if (wc_InitSha256(&hash) != 0) return 500;
  ScopedCleanup freeHash{[&] { wc_Sha256Free(&hash); }};
  if (request.expectContinue) client.print("HTTP/1.1 100 Continue\r\n\r\n");
  session.state = localsend::State::Receiving;
  [[maybe_unused]] const uint32_t started = millis();  // Used by device logging; host stub suppresses logs.
  uint8_t buffer[1024], magic[8]{};
  uint32_t chunks = 0;
  while (true) {
    uint32_t remaining = offered->expected - offered->received;
    if (request.chunked) {
      char line[129];
      if (++chunks > 4u * 1024u * 1024u) {
        LOG_ERR("LSEND", "Upload exceeded chunk limit: %s", offered->fileName);
        return 400;
      }
      const int result = receiveLine(client, line, sizeof(line), millis());
      if (result != 200) {
        LOG_ERR("LSEND", "Chunk header failed: %s code=%d received=%u/%u connected=%u", offered->fileName, result,
                unsigned(offered->received), unsigned(offered->expected), unsigned(client.connected()));
        return result;
      }
      uint32_t size;
      if (!localsend::parseChunkSize(line, size) || size > remaining) {
        LOG_ERR("LSEND", "Chunk size invalid: %s received=%u/%u", offered->fileName, unsigned(offered->received),
                unsigned(offered->expected));
        return 400;
      }
      if (!size) {
        size_t trailerBytes = 0;
        bool ended = false;
        for (unsigned trailer = 0; trailer < 8; ++trailer) {
          const int read = receiveLine(client, line, sizeof(line), millis());
          if (read != 200) return read;
          trailerBytes += std::strlen(line) + 2;
          if (trailerBytes > 1024) return 400;
          if (!*line) {
            ended = true;
            break;
          }
          if (!std::strchr(line, ':')) return 400;
        }
        if (!ended) return 400;
        break;
      }
      remaining = size;
    } else if (!remaining)
      break;
    while (remaining) {
      const size_t take = std::min(sizeof(buffer), size_t(remaining));
      if (!receive(client, buffer, take, millis(), 600000, 30000)) {
        LOG_ERR("LSEND", "Upload read stopped: %s received=%u/%u connected=%u elapsed=%u ms", offered->fileName,
                unsigned(offered->received), unsigned(offered->expected), unsigned(client.connected()),
                unsigned(millis() - started));
        return 408;
      }
      if (offered->received < sizeof(magic))
        std::memcpy(magic + offered->received, buffer, std::min(take, sizeof(magic) - size_t(offered->received)));
      if (!session.consume(*offered, take, millis()) || file.write(buffer, take) != take ||
          wc_Sha256Update(&hash, buffer, word32(take)) != 0)
        return 500;
      remaining -= uint32_t(take);
    }
    if (request.chunked) {
      uint8_t end[2];
      if (!receive(client, end, sizeof(end), millis(), 600000, 30000)) return 408;
      if (end[0] != '\r' || end[1] != '\n') return 400;
    }
  }
  if (!session.complete(*offered)) {
    LOG_ERR("LSEND", "Upload incomplete for %s: received=%u expected=%u", offered->fileName,
            unsigned(offered->received), unsigned(offered->expected));
    return 422;
  }
  if (!localsend::validFileSignature(offered->fileName, magic, std::min<size_t>(offered->received, sizeof(magic)))) {
    LOG_ERR("LSEND", "File signature mismatch: %s starts %02X %02X %02X %02X", offered->fileName, unsigned(magic[0]),
            unsigned(magic[1]), unsigned(magic[2]), unsigned(magic[3]));
    return 422;
  }
  uint8_t digest[32];
  char hex[65];
  if (wc_Sha256Final(&hash, digest) != 0) return 500;
  hexDigest(digest, hex);
  if (offered->sha256[0] && std::strcmp(offered->sha256, hex)) {
    LOG_ERR("LSEND", "SHA-256 mismatch for %s (offered %.12s, received %.12s)", offered->fileName, offered->sha256,
            hex);
    return 422;
  }
  if (file.fileSize64() != offered->expected || !file.close()) return 500;
  if (cancelled()) return 408;
  if (Storage.exists(path)) {
    LOG_ERR("LSEND", "Upload publication rejected: file already exists at %s (preserved)", path);
    return 409;
  }
  if (!Storage.rename(temporary, path)) {
    LOG_ERR("LSEND", "Upload publication failed: cannot rename staging file to %s", path);
    return 500;
  }
  published = true;
  std::memcpy(savedName, offered->fileName, sizeof(savedName));
  savedBytes = offered->received;
  ++savedSequence;
  LOG_INF("LSEND", "Saved %s (%u bytes)", path, unsigned(offered->received));
  const bool finished = session.finishFile(*offered, millis());
  succeeded = true;
  if (finished) session.reset();
  return 200;
}
void LocalSendReceiver::announce(bool initial) {
  lastAnnouncement = millis();
  if (discoverySocket < 0) return;
  localsend::BoundedJsonAllocator allocator;
  JsonDocument doc(&allocator);
  deviceInfo(doc, fingerprint, advertisedAlias);
  doc["announce"] = initial;
  char bytes[384];
  const size_t length = serializeJson(doc, bytes, sizeof(bytes));
  if (length >= sizeof(bytes)) return;
  sockaddr_in group{};
  group.sin_family = AF_INET;
  group.sin_port = htons(PORT);
  group.sin_addr.s_addr = inet_addr(GROUP);
  sendto(discoverySocket, bytes, length, 0, reinterpret_cast<sockaddr*>(&group), sizeof(group));
}
void LocalSendReceiver::discovery() {
  if (discoverySocket < 0) return;
  char bytes[1025];
  const int n = recv(discoverySocket, bytes, sizeof(bytes), 0);
  if (n <= 0 || size_t(n) >= sizeof(bytes)) return;
  localsend::BoundedJsonAllocator allocator;
  JsonDocument doc(&allocator);
  if (deserializeJson(doc, bytes, size_t(n), DeserializationOption::NestingLimit(4))) return;
  if (doc["announce"].as<bool>() && doc["fingerprint"].is<const char*>() &&
      std::strcmp(doc["fingerprint"].as<const char*>(), fingerprint) && millis() - lastReply >= 250) {
    lastReply = millis();
    announce(false);
  }
}
const localsend::Session* LocalSendReceiver::pendingOffer() const {
  return session.state == localsend::State::AwaitingConsent ? &session : nullptr;
}
void LocalSendReceiver::pollConsent() {
  if (!pendingOffer()) return;
  if (WiFi.status() != WL_CONNECTED || !pendingClient.connected() ||
      millis() - pendingSince >= localsend::CONSENT_TIMEOUT_MS)
    decide(false);
}
void LocalSendReceiver::decide(bool accept) {
  if (!pendingOffer()) return;
  int code = 403;
  const bool online = WiFi.status() == WL_CONNECTED;
  const bool senderConnected = pendingClient.connected();
  const bool timedOut = millis() - pendingSince >= localsend::CONSENT_TIMEOUT_MS;
  const char* rejectionReason = !online            ? "WiFi disconnected"
                                : !senderConnected ? "Sender disconnected"
                                : timedOut         ? "Transfer acceptance timed out"
                                : !accept          ? "Transfer declined"
                                                   : "File already exists in destination folder";
  if (accept && online && senderConnected && !timedOut) {
    code = 200;
    for (uint8_t i = 0; i < session.fileCount; ++i) {
      char path[256];
      std::snprintf(path, sizeof(path), "%s%s%s", destination, std::strcmp(destination, "/") ? "/" : "",
                    session.files[i].fileName);
      if (Storage.exists(path)) {
        LOG_ERR("LSEND", "Transfer acceptance rejected: file already exists at %s (preserved)", path);
        code = 403;
        break;
      }
    }
  }
  if (code == 200) {
    const uint64_t cardBytes = Storage.sdTotalBytes();
    uint64_t usedBytes = 0;
    const bool spaceKnown = Storage.sdUsedBytes(usedBytes);
    // Total/used are both whole FAT/exFAT clusters; their GCD is a safe upper bound
    // on the allocation unit. Cap it at exFAT's 32 MiB maximum.
    constexpr uint64_t MAX_ALLOC_UNIT = 32ull * 1024 * 1024;
    const uint64_t unit = std::max<uint64_t>(1, std::min(std::gcd(cardBytes, usedBytes), MAX_ALLOC_UNIT));
    // Reserve clusters for staging/destination directory growth, including possible parent creation.
    uint64_t requestedBytes = unit * (3 + std::count(destination, destination + std::strlen(destination), '/'));
    for (uint8_t i = 0; i < session.fileCount; ++i) {
      const uint64_t size = session.files[i].expected;
      requestedBytes += ((size + unit - 1) / unit + 2) * unit;  // File data and two directory entries.
    }
    if (!cardBytes || !spaceKnown || usedBytes > cardBytes) {
      LOG_ERR("LSEND", "Transfer acceptance rejected: could not verify free SD card space");
      rejectionReason = "Unable to verify free space on SD card";
      code = 500;
    } else if (requestedBytes > cardBytes - usedBytes) {
      LOG_ERR("LSEND", "Transfer acceptance rejected: SD has insufficient space for %llu bytes",
              static_cast<unsigned long long>(requestedBytes));
      rejectionReason = "Not enough free space on SD card";
      code = 507;
    }
  }
  if (code == 200) {
    localsend::BoundedJsonAllocator allocator;
    JsonDocument doc(&allocator);
    doc["sessionId"] = session.sessionId;
    for (uint8_t i = 0; i < session.fileCount; ++i) {
      randomId(session.files[i].token);
      doc["files"][session.files[i].fileId] = session.files[i].token;
    }
    constexpr size_t MAX_RESPONSE = 4096;
    const size_t responseSize = measureJson(doc);
    auto response =
        (!doc.overflowed() && responseSize < MAX_RESPONSE) ? makeUniqueNoThrow<char[]>(responseSize + 1) : nullptr;
    if (!response) {
      if (!doc.overflowed() && responseSize < MAX_RESPONSE)
        LOG_ERR("LSEND", "OOM: %u-byte consent response", unsigned(responseSize + 1));
      code = 500;
      reply(pendingClient, code);
    } else {
      serializeJson(doc, response.get(), responseSize + 1);
      session.state = localsend::State::Prepared;
      session.lastActivity = millis();
      reply(pendingClient, 200, response.get());
    }
  } else {
    LOG_ERR("LSEND", "Transfer rejected: %u file(s), first %s (%s)", unsigned(session.fileCount),
            session.files[0].fileName, rejectionReason);
    char response[160];
    std::snprintf(response, sizeof(response), "{\"message\":\"%s\"}", rejectionReason);
    reply(pendingClient, code, response);
  }
  LOG_INF("LSEND", "Transfer acceptance: HTTP %d", code);
  pendingClient.stop();
  pendingClient = NetworkClient{};
  if (code != 200) session.reset();
}
void LocalSendReceiver::poll() {
  pollConsent();
  if (pendingOffer()) return;
  if (!listener || WiFi.status() != WL_CONNECTED) return;
  if (session.expired(millis())) session.reset();
  discovery();
  if (millis() - lastAnnouncement >= 10000) announce(true);
  auto client = listener.accept();
  if (!client) return;
  serve(client);
  if (!pendingOffer()) client.stop();
}
#endif
