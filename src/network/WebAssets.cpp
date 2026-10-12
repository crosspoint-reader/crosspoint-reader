#include "WebAssets.h"

#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>
#include <WebServer.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "HttpFileStreamer.h"
#include "OtaUpdater.h"  // CROSSPOINT_GIT_REPOSITORY and its fallback
#include "network/HttpDownloader.h"

// Set per environment by scripts/build_html.py; empty for unpublished builds.
#ifndef WEB_BUNDLE_RELEASE_TAG
#define WEB_BUNDLE_RELEASE_TAG ""
#endif

namespace WebAssets {
namespace {

constexpr char WEB_DIR[] = "/.crosspoint/web";
// Unpacked here first and renamed over WEB_DIR only once complete, so a cut
// install never replaces a working set.
constexpr char STAGING_DIR[] = "/.crosspoint/web.new";
constexpr char DOWNLOAD_PATH[] = "/.crosspoint/web-download.bin";
// Holds the installed bundle's SHA-256.
constexpr char STAMP_FILE[] = "bundle.id";
constexpr size_t COPY_CHUNK = 2048;

bool s_stateKnown = false;
State s_state = State::Missing;

void joinPath(char* out, size_t outSize, const char* dir, const char* name) {
  snprintf(out, outSize, "%s/%s", dir, name);
}

State readState() {
  char path[48];
  joinPath(path, sizeof(path), WEB_DIR, STAMP_FILE);
  if (!Storage.exists(path)) return State::Missing;
  FsFile f;
  if (!Storage.openFileForRead("WEB", path, f)) return State::Missing;
  uint8_t stamp[sizeof(web_bundle::DIGEST)];
  const int got = f.read(stamp, sizeof(stamp));
  f.close();
  if (got != static_cast<int>(sizeof(stamp))) return State::Stale;
  return memcmp(stamp, web_bundle::DIGEST, sizeof(stamp)) == 0 ? State::Installed : State::Stale;
}

void setState(const State state) {
  s_state = state;
  s_stateKnown = true;
}

std::string cardBundlePath() { return std::string("/") + web_bundle::ASSET_NAME; }

// Pass 1: the whole file must be this build's bundle before anything is written.
InstallError verify(FsFile& in, uint8_t* buf) {
  if (in.size() != web_bundle::TOTAL_SIZE) {
    LOG_ERR("WEB", "bundle is %u B, this build's is %u B", static_cast<unsigned>(in.size()),
            static_cast<unsigned>(web_bundle::TOTAL_SIZE));
    return InstallError::WrongBundle;
  }
  wc_Sha256 sha;
  if (wc_InitSha256(&sha) != 0) return InstallError::OutOfMemory;
  bool readOk = true;
  size_t remaining = web_bundle::TOTAL_SIZE;
  while (readOk && remaining > 0) {
    HalSystem::feedWatchdog();
    const size_t want = std::min(COPY_CHUNK, remaining);
    readOk = in.read(buf, want) == static_cast<int>(want);
    if (readOk) wc_Sha256Update(&sha, buf, static_cast<word32>(want));
    remaining -= want;
  }
  uint8_t digest[sizeof(web_bundle::DIGEST)];
  wc_Sha256Final(&sha, digest);
  // Every path through here frees: with hardware SHA the context can hold the engine.
  wc_Sha256Free(&sha);
  if (!readOk) return InstallError::CardError;
  if (memcmp(digest, web_bundle::DIGEST, sizeof(digest)) != 0) {
    LOG_ERR("WEB", "bundle hash differs from this build's %s", web_bundle::ASSET_NAME);
    return InstallError::WrongBundle;
  }
  return InstallError::None;
}

// Pass 2: split the verified bundle into its files under STAGING_DIR, stamp it,
// then swap it in for the live set.
InstallError unpack(FsFile& in, uint8_t* buf) {
  if (Storage.exists(STAGING_DIR)) Storage.removeDir(STAGING_DIR);  // left by a cut install
  if (!Storage.mkdir(STAGING_DIR)) return InstallError::CardError;
  if (!in.seek(0)) return InstallError::CardError;

  char path[64];
  for (const web_bundle::Entry& entry : web_bundle::ENTRIES) {
    joinPath(path, sizeof(path), STAGING_DIR, entry.file);
    FsFile out;
    if (!Storage.openFileForWrite("WEB", path, out)) return InstallError::CardError;
    size_t remaining = entry.size;
    while (remaining > 0) {
      HalSystem::feedWatchdog();
      const size_t want = std::min(COPY_CHUNK, remaining);
      if (in.read(buf, want) != static_cast<int>(want) || out.write(buf, want) != want) {
        out.close();
        return InstallError::CardError;
      }
      remaining -= want;
    }
    out.close();
  }

  joinPath(path, sizeof(path), STAGING_DIR, STAMP_FILE);
  FsFile stamp;
  if (!Storage.openFileForWrite("WEB", path, stamp)) return InstallError::CardError;
  const size_t wrote = stamp.write(web_bundle::DIGEST, sizeof(web_bundle::DIGEST));
  stamp.close();
  if (wrote != sizeof(web_bundle::DIGEST)) return InstallError::CardError;

  // A power cut between these two leaves no WEB_DIR: Missing, and reinstalled.
  if (Storage.exists(WEB_DIR) && !Storage.removeDir(WEB_DIR)) return InstallError::CardError;
  if (!Storage.rename(STAGING_DIR, WEB_DIR)) return InstallError::CardError;
  return InstallError::None;
}

InstallError installFromFile(const char* bundlePath) {
  if (!Storage.exists(bundlePath)) return InstallError::NotFound;
  // Transient: freed before this returns. 2 KB keeps it a small block on a
  // web-session heap; the bundle is ~75 KB, so the chunk size costs no time.
  auto buf = makeUniqueNoThrow<uint8_t[]>(COPY_CHUNK);
  if (!buf) {
    LOG_ERR("WEB", "OOM: %u B bundle copy buffer", static_cast<unsigned>(COPY_CHUNK));
    return InstallError::OutOfMemory;
  }
  FsFile in;
  if (!Storage.openFileForRead("WEB", bundlePath, in)) return InstallError::NotFound;
  InstallError err = verify(in, buf.get());
  if (err == InstallError::None) err = unpack(in, buf.get());
  in.close();

  if (err != InstallError::None) {
    if (Storage.exists(STAGING_DIR)) Storage.removeDir(STAGING_DIR);
    LOG_ERR("WEB", "installing %s failed: %s", bundlePath, errorText(err));
    s_stateKnown = false;  // the live set may be gone if the swap failed halfway
    return err;
  }
  setState(State::Installed);
  LOG_INF("WEB", "web interface %s installed (%u files)", web_bundle::ASSET_NAME,
          static_cast<unsigned>(web_bundle::ENTRY_COUNT));
  return InstallError::None;
}

}  // namespace

State state() {
  if (!s_stateKnown) setState(readState());
  return s_state;
}

void forget() { s_stateKnown = false; }

const char* stateName(const State state) {
  switch (state) {
    case State::Installed:
      return "installed";
    case State::Missing:
      return "missing";
    case State::Stale:
      return "stale";
  }
  return "missing";
}

bool serve(WebServer& server, const web_bundle::Index index) {
  if (state() != State::Installed) return false;

  // The bundle's name holds the hash of its contents, so it stands for every file in it. The
  // headers go out only with an answer: a false return leaves the response to the caller.
  char etag[sizeof(web_bundle::ASSET_NAME) + 2];
  snprintf(etag, sizeof(etag), "\"%s\"", web_bundle::ASSET_NAME);
  if (server.header("If-None-Match") == etag) {
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("ETag", etag);
    server.send(304);
    return true;
  }

  const web_bundle::Entry& entry = web_bundle::ENTRIES[index];
  char path[64];
  joinPath(path, sizeof(path), WEB_DIR, entry.file);
  FsFile f;
  if (!Storage.exists(path) || !Storage.openFileForRead("WEB", path, f)) {
    LOG_ERR("WEB", "%s is missing - the web interface needs reinstalling", path);
    setState(State::Missing);
    return false;
  }
  if (f.size() != entry.size) {
    LOG_ERR("WEB", "%s is %u B, expected %u B - the web interface needs reinstalling", path,
            static_cast<unsigned>(f.size()), static_cast<unsigned>(entry.size));
    f.close();
    setState(State::Missing);
    return false;
  }

  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("ETag", etag);
  server.setContentLength(entry.size);
  server.send(200, entry.contentType, "");
  NetworkClient client = server.client();
  if (!HttpFileStreamer::streamFileToClient(f, client)) {
    LOG_DBG("WEB", "streaming %s interrupted", entry.file);
  }
  client.clear();
  f.close();
  return true;
}

bool serveScript(WebServer& server, const char* name) {
  const size_t len = strlen(name);
  for (size_t i = 0; i < web_bundle::ENTRY_COUNT; i++) {
    const web_bundle::Entry& entry = web_bundle::ENTRIES[i];
    if (strcmp(entry.contentType, "text/html") == 0) continue;  // pages have routes of their own
    if (strncmp(entry.file, name, len) == 0 && strcmp(entry.file + len, ".gz") == 0) {
      return serve(server, static_cast<web_bundle::Index>(i));
    }
  }
  return false;
}

const char* releaseTag() { return WEB_BUNDLE_RELEASE_TAG; }

const char* repository() { return CROSSPOINT_GIT_REPOSITORY; }

const char* assetName() { return web_bundle::ASSET_NAME; }

bool bundleOnCard() { return Storage.exists(cardBundlePath().c_str()); }

InstallError installFromRelease() {
  // cppcheck-suppress knownConditionTrueFalse ; compile-time true in unpublished builds
  if (releaseTag()[0] == '\0') return InstallError::NoRelease;

  const std::string url = std::string("https://github.com/") + repository() + "/releases/download/" + releaseTag() +
                          "/" + web_bundle::ASSET_NAME;
  LOG_INF("WEB", "downloading the web interface: %s", url.c_str());
  Storage.mkdir("/.crosspoint");
  // Unverified TLS on purpose: the bytes are checked against the SHA-256 built
  // into this firmware, which authenticates them more strictly than a
  // certificate chain would, and skipping the chain verify keeps the handshake
  // cheap on a web-session heap.
  const HttpDownloader::DownloadError result =
      HttpDownloader::downloadToFile(url, DOWNLOAD_PATH, nullptr, "", "", HttpDownloader::TlsPolicy::Unverified);
  if (result != HttpDownloader::OK) {
    LOG_ERR("WEB", "web interface download failed (%d)", static_cast<int>(result));
    Storage.remove(DOWNLOAD_PATH);
    return InstallError::Download;
  }
  const InstallError err = installFromFile(DOWNLOAD_PATH);
  Storage.remove(DOWNLOAD_PATH);
  return err;
}

InstallError installFromCard() {
  const std::string path = cardBundlePath();
  const InstallError err = installFromFile(path.c_str());
  // A wrong bundle stays where the user put it; they may want to see which
  // file it was. An installed one has done its job.
  if (err == InstallError::None) Storage.remove(path.c_str());
  return err;
}

const char* errorText(const InstallError error) {
  switch (error) {
    case InstallError::None:
      return "ok";
    case InstallError::NoRelease:
      return "this build has no release to download from";
    case InstallError::Download:
      return "the download from GitHub failed";
    case InstallError::NotFound:
      return "no bundle file on the SD card";
    case InstallError::WrongBundle:
      return "the file is not the bundle for this firmware";
    case InstallError::CardError:
      return "writing the SD card failed";
    case InstallError::OutOfMemory:
      return "not enough memory";
  }
  return "unknown error";
}

}  // namespace WebAssets
