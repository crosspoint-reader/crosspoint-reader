#include "BleSyncManager.h"

#include <Arduino.h>  // millis
#include <WiFi.h>

#include <algorithm>

#include "BleProgressBridge.h"
#include "BleSyncProtocol.h"
#include "BleSyncService.h"
#include "KOReaderCredentialStore.h"  // getBleSyncEnabled gate
#include "KOReaderSyncClient.h"       // KOReaderProgress

namespace proto = BleSyncProtocol;

namespace {
constexpr unsigned long kResendMs = 700;       // resend our MANIFEST / un-ACKed PROGRESS
constexpr unsigned long kSettleMs = 1500;      // quiet window before we call the sync done
constexpr unsigned long kResultShowMs = 4000;  // how long the ✓/✗ glyph lingers
constexpr unsigned long kFreshMs = 90000;      // "super recent" window — skip the book-open wait
constexpr size_t kMaxManifest = 20;            // most-recent books considered per reconcile
constexpr size_t kManifestPageSize = 4;        // conservative JSON page that fits one ATT MTU 517 notification
constexpr int kMaxPushAttempts = 3;            // resend a PROGRESS up to 3x (PROTOCOL-v3 §4)

// Singleton state (only one Manager exists).
BleSync::Phase g_phase = BleSync::Phase::Idle;
GfxRenderer* g_renderer = nullptr;
bool g_blocking = false;

unsigned long g_startMs = 0;
unsigned long g_deadlineMs = 0;
unsigned long g_lastActivityMs = 0;
unsigned long g_lastManifestMs = 0;
unsigned long g_finishedAt = 0;
unsigned long g_lastSuccessAt = 0;

bool g_everConnected = false;
bool g_pushedOpen = false;
bool g_sentManifest = false;
bool g_gotPhoneManifest = false;
bool g_phoneManifestComplete = false;  // phone's manifest ended with more=false (§6)
bool g_wasConnected = false;
std::vector<proto::ManifestEntry> g_phoneManifest;

// Books we've pushed this run, with resend bookkeeping (PROTOCOL-v3 §4).
struct PushRec {
  std::string key;
  int attempts;
  unsigned long lastMs;
};
std::vector<PushRec> g_pushes;
int g_booksSent = 0;
int g_booksApplied = 0;
int g_bookCount = 0;
uint8_t g_spin = 0;
bool g_result = false;
std::string g_error;
bool g_dirty = false;

void bump() {
  g_spin = static_cast<uint8_t>((g_spin + 1) & 0x07);  // 8-step spinner
  g_dirty = true;
}

void resetRun() {
  g_everConnected = false;
  g_pushedOpen = false;
  g_sentManifest = false;
  g_gotPhoneManifest = false;
  g_phoneManifestComplete = false;
  g_wasConnected = false;
  g_phoneManifest.clear();
  g_pushes.clear();
  g_booksSent = 0;
  g_booksApplied = 0;
  g_bookCount = 0;
  g_lastManifestMs = 0;
}

int overallPercent() {
  const int done = g_booksSent + g_booksApplied;
  if (g_bookCount <= 0) return g_gotPhoneManifest ? 100 : 0;
  return std::min(100, done * 100 / g_bookCount);
}

// Finish the run: release the radio, latch the result, start the glyph timer.
void finishWith(BleSync::Phase phase, bool ok, const std::string& err) {
  BleSyncService::instance().stop();  // release BLE radio + heap
  g_phase = phase;
  g_result = ok;
  g_error = err;
  g_finishedAt = millis();
  if (ok) g_lastSuccessAt = g_finishedAt;
  g_dirty = true;
}

// Load a recent book's saved position and notify one PROGRESS. Heavy (loads the
// epub). Returns false if the book can't be resolved/loaded/has no progress.
bool sendProgressForKey(const std::string& titleHash) {
  if (titleHash.empty()) return false;
  const std::string path = BleProgress::pathForHash("", titleHash);
  if (path.empty()) return false;
  KOReaderProgress kp;
  std::string th;
  if (!BleProgress::getForPath(path, kp, th, /*deviceId=*/"x4")) return false;
  BleSyncService::instance().sendProgress(kp.document, th, kp.progress, kp.percentage, kp.timestamp);
  return true;
}

// First push of a book this run (deduped per key). Records it for resend.
void pushProgressFor(const std::string& titleHash) {
  if (titleHash.empty()) return;
  for (const auto& r : g_pushes) {
    if (r.key == titleHash) return;  // already pushing this run
  }
  if (!sendProgressForKey(titleHash)) return;
  g_pushes.push_back({titleHash, 1, millis()});
  g_booksSent++;
  g_lastActivityMs = millis();
  bump();
}

// Resend PROGRESS the phone hasn't ACKed yet (subscribe-race / RF drop). Stops
// once the phone's ACK count catches up to what we've sent, or the per-book
// attempt cap is hit (PROTOCOL-v3 §4).
void resendUnackedPushes() {
  auto& ble = BleSyncService::instance();
  if (ble.receivedAckCount() >= g_booksSent) return;  // all delivered
  const unsigned long now = millis();
  for (auto& r : g_pushes) {
    if (r.attempts >= kMaxPushAttempts) continue;
    if (now - r.lastMs < kResendMs) continue;
    if (sendProgressForKey(r.key)) {
      r.attempts++;
      r.lastMs = now;
      g_lastActivityMs = now;
      bump();
    }
  }
}

void sendLocalManifest() {
  auto& ble = BleSyncService::instance();
  const auto books = BleProgress::buildLocalManifest(kMaxManifest);
  if (books.empty()) {
    ble.sendManifest({}, /*more=*/false);
    return;
  }
  for (size_t offset = 0; offset < books.size(); offset += kManifestPageSize) {
    const size_t end = std::min(books.size(), offset + kManifestPageSize);
    const std::vector<proto::ManifestEntry> page(books.begin() + offset, books.begin() + end);
    ble.sendManifest(page, /*more=*/end < books.size());
  }
}

void appendPhoneManifestPage(const std::vector<proto::ManifestEntry>& page) {
  for (const auto& incoming : page) {
    const auto existing = std::find_if(g_phoneManifest.begin(), g_phoneManifest.end(), [&incoming](const auto& book) {
      return (!incoming.titleHash.empty() && book.titleHash == incoming.titleHash) ||
             (incoming.titleHash.empty() && !incoming.document.empty() && book.document == incoming.document);
    });
    if (existing == g_phoneManifest.end()) {
      g_phoneManifest.push_back(incoming);
    } else {
      *existing = incoming;
    }
  }
}
}  // namespace

namespace BleSync {

Manager& Manager::instance() {
  static Manager m;
  return m;
}

bool Manager::start(GfxRenderer& renderer, unsigned long deadlineMs, bool blocking) {
  if (isActive()) return false;
  if (!KOREADER_STORE.getBleSyncEnabled()) return false;

  g_renderer = &renderer;
  g_blocking = blocking;
  g_deadlineMs = deadlineMs;
  g_startMs = millis();
  g_lastActivityMs = g_startMs;
  resetRun();
  g_error.clear();

  // BLE and Wi-Fi cannot coexist on the ESP32-C3 — drop Wi-Fi before BLE.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  auto& ble = BleSyncService::instance();
  ble.stop();
  ble.begin("");  // advertise as x4-<mac>
  g_phase = Phase::Advertising;
  g_dirty = true;
  return true;
}

void Manager::stop() {
  if (g_phase == Phase::Advertising || g_phase == Phase::Syncing) {
    BleSyncService::instance().stop();
    g_phase = Phase::Idle;
    g_finishedAt = millis();
    g_dirty = true;
  }
}

bool Manager::isActive() const { return g_phase == Phase::Advertising || g_phase == Phase::Syncing; }
bool Manager::isBlocking() const { return isActive() && g_blocking; }

void Manager::loop() {
  if (!isActive()) return;

  auto& ble = BleSyncService::instance();
  const bool connected = ble.isConnected();

  // Hard deadline (PROTOCOL-v3 §2): no early no-phone give-up — the window
  // advertises the whole time so a phone that takes a few seconds to reconnect
  // still gets in. No phone by the deadline → finish quietly as Idle (below).
  if (millis() - g_startMs > g_deadlineMs) {
    if (g_everConnected && g_phoneManifestComplete) {
      finishWith(Phase::Success, true, "");  // exchanged books, just ran long
    } else if (g_everConnected) {
      finishWith(Phase::Failed, false, "sync timed out mid-exchange");
    } else {
      finishWith(Phase::Idle, false, "");  // no phone
    }
    return;
  }

  // Fresh connection: move to Syncing, arm the reconcile.
  if (connected && !g_wasConnected) {
    g_everConnected = true;
    resetRun();
    g_everConnected = true;  // resetRun cleared it
    g_phase = Phase::Syncing;
    g_lastActivityMs = millis();
    bump();
  }
  if (connected && g_renderer) {
    // Send our MANIFEST; resend until the phone replies. The per-book PROGRESS
    // pushes happen only AFTER we receive the phone's manifest (below), which
    // means the phone has already subscribed — so a single send is reliable. (An
    // earlier one-shot "fast-path" push of the open book raced the subscription
    // and, worse, marked the book pushed so the reliable path was deduped away.)
    const bool manifestDue = !g_sentManifest || (!g_gotPhoneManifest && millis() - g_lastManifestMs > kResendMs);
    if (manifestDue) {
      sendLocalManifest();
      g_sentManifest = true;
      g_lastManifestMs = millis();
      g_lastActivityMs = millis();
    }
  }

  // Drain inbound messages even after the peer disconnects: the final write
  // and the disconnect callback can arrive before this main-loop task runs.
  if (g_renderer) {
    proto::ParsedMessage m;
    while (ble.takeReceivedMessage(m)) {
      g_lastActivityMs = millis();
      if (m.type == proto::kTypeManifest) {
        g_gotPhoneManifest = true;
        appendPhoneManifestPage(m.books);
        g_phoneManifestComplete = !m.more;  // wait for the last page before settling (§6)
        if (!g_phoneManifestComplete) continue;
        const auto mine = BleProgress::buildLocalManifest(kMaxManifest);
        // Count books that need syncing in EITHER direction → the "x" in "i of x".
        int diff = 0;
        for (const auto& lb : mine) {
          int64_t phoneTs = -1;
          for (const auto& pb : g_phoneManifest) {
            if (pb.titleHash == lb.titleHash) {
              phoneTs = pb.updatedAt;
              break;
            }
          }
          if (lb.updatedAt > phoneTs || (phoneTs > lb.updatedAt && phoneTs > 0)) diff++;
        }
        // Books only the phone has that are newer than anything local.
        for (const auto& pb : g_phoneManifest) {
          bool inMine = false;
          for (const auto& lb : mine) {
            if (lb.titleHash == pb.titleHash) {
              inMine = true;
              break;
            }
          }
          if (!inMine && pb.updatedAt > 0) diff++;
        }
        g_bookCount = diff;
        // Push every local book we're strictly newer on (or the phone lacks).
        for (const auto& lb : mine) {
          if (lb.updatedAt <= 0) continue;
          int64_t phoneTs = -1;
          for (const auto& pb : g_phoneManifest) {
            if (pb.titleHash == lb.titleHash) {
              phoneTs = pb.updatedAt;
              break;
            }
          }
          if (lb.updatedAt > phoneTs) pushProgressFor(lb.titleHash);
        }
        bump();
      } else if (m.type == proto::kTypeProgress) {
        KOReaderProgress kp;
        kp.document = m.document;
        kp.progress = m.xpointer;
        kp.percentage = m.percentage;
        kp.timestamp = m.updatedAt;
        if (BleProgress::applyRemote(kp, m.titleHash, *g_renderer)) {
          g_booksApplied++;
          bump();
        }
      } else if (m.type == proto::kTypeWant) {
        for (const auto& k : m.wantKeys) pushProgressFor(k);
      }
    }
  }

  if (connected) resendUnackedPushes();

  // Only decide that a disconnect failed after consuming everything received
  // before it. A complete final manifest or PROGRESS must not be discarded.
  if (!connected && g_wasConnected && !g_phoneManifestComplete) {
    finishWith(Phase::Failed, false, "BLE peer disconnected during sync");
    return;
  }
  g_wasConnected = connected;

  // Settle: both manifests fully exchanged (phone's ended with more=false) and
  // the line has gone quiet → done.
  if (g_sentManifest && g_gotPhoneManifest && g_phoneManifestComplete && millis() - g_lastActivityMs > kSettleMs) {
    finishWith(Phase::Success, true, "");
  }
}

Status Manager::status() const {
  Status s;
  s.phase = g_phase;
  s.bookIndex = g_booksSent + g_booksApplied;
  s.bookCount = g_bookCount;
  s.percent = overallPercent();
  s.spin = g_spin;
  s.result = g_result;
  s.error = g_error;
  return s;
}

bool Manager::statusDirty() {
  const bool d = g_dirty;
  g_dirty = false;
  return d;
}

bool Manager::hasFreshResult() const {
  if (g_phase != Phase::Success && g_phase != Phase::Failed) return false;
  return millis() - g_finishedAt < kResultShowMs;
}

unsigned long Manager::lastSuccessMs() const { return g_lastSuccessAt; }
const std::string& Manager::lastError() const { return g_error; }

bool shouldSyncBeforeOpen() {
  if (!KOREADER_STORE.getBleSyncEnabled()) return false;
  if (Manager::instance().isActive()) return true;  // keep the book behind the wait until this run settles
  const unsigned long last = Manager::instance().lastSuccessMs();
  if (last == 0) return true;            // never synced this boot → sync now
  return (millis() - last) >= kFreshMs;  // stale → sync; super-recent → skip
}

}  // namespace BleSync
