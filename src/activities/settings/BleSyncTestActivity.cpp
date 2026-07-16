#include "BleSyncTestActivity.h"

#include <Arduino.h>  // millis
#include <WiFi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "KOReaderSyncClient.h"  // KOReaderProgress
#include "ble_sync/BleProgressBridge.h"
#include "ble_sync/BleSyncProtocol.h"
#include "ble_sync/BleSyncService.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace proto = BleSyncProtocol;

namespace {
constexpr unsigned long kResendMs = 700;    // resend our MANIFEST until the phone replies
constexpr unsigned long kSettleMs = 1500;   // quiet window before we call the sync done
constexpr unsigned long kNoPhoneMs = 8000;  // auto-trigger: give up if no phone ever connects
constexpr size_t kMaxManifest = 20;         // books per MANIFEST (fits MTU 517)
}  // namespace

BleSyncTestActivity::BleSyncTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         unsigned long autoExitMs)
    : Activity("BLE Sync", renderer, mappedInput), autoExitMs_(autoExitMs) {}

void BleSyncTestActivity::armReconcile() {
  pushedOpen_ = false;
  sentManifest_ = false;
  gotPhoneManifest_ = false;
  pushedKeys_.clear();
  booksSent_ = 0;
  booksApplied_ = 0;
  lastManifestMs_ = 0;
  lastActivityMs_ = millis();
}

void BleSyncTestActivity::startAdvertising() {
  // BLE and Wi-Fi cannot coexist on the ESP32-C3 — drop Wi-Fi before BLE.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  auto& ble = BleSyncService::instance();
  ble.stop();
  ble.begin("");  // advertise as x4-<mac>
  state_ = State::Advertising;
  everConnected_ = false;
  armReconcile();
  statusSummary_.clear();
}

void BleSyncTestActivity::onEnter() {
  Activity::onEnter();
  enterMs_ = millis();
  startAdvertising();
  requestUpdate();
}

void BleSyncTestActivity::onExit() {
  Activity::onExit();
  BleSyncService::instance().stop();  // release the radio
}

// Send one recent book's PROGRESS (loads the epub — heavy; deduped per key).
void BleSyncTestActivity::pushProgressFor(const std::string& titleHash) {
  if (titleHash.empty()) return;
  for (const auto& k : pushedKeys_) {
    if (k == titleHash) return;  // already sent this connection
  }
  const std::string path = BleProgress::pathForHash("", titleHash);
  if (path.empty()) return;
  KOReaderProgress kp;
  std::string th;
  if (!BleProgress::getForPath(path, kp, th, /*deviceId=*/"x4")) return;
  BleSyncService::instance().sendProgress(kp.document, th, kp.progress, kp.percentage, kp.timestamp);
  pushedKeys_.push_back(titleHash);
  booksSent_++;
  lastActivityMs_ = millis();
}

void BleSyncTestActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Auto-exit (boot/exit triggers): give up early if no phone is in range, time
  // out, or finish once the reconcile settles.
  if (autoExitMs_ > 0) {
    const bool noPhone = !everConnected_ && (millis() - enterMs_ > kNoPhoneMs);
    const bool timedOut = millis() - enterMs_ > autoExitMs_;
    const bool settled = sentManifest_ && gotPhoneManifest_ && (millis() - lastActivityMs_ > kSettleMs);
    if (noPhone || timedOut || settled) {
      finish();
      return;
    }
  }

  auto& ble = BleSyncService::instance();
  const bool connected = ble.isConnected();
  const State next = connected ? State::Connected : State::Advertising;
  bool changed = (next != state_);

  // Re-arm the reconcile on each fresh connection.
  if (next == State::Connected && state_ == State::Advertising) {
    everConnected_ = true;
    armReconcile();
    changed = true;
  }

  if (connected) {
    // 1. v1 fast-path: push the open/last-read book once (works even with a v1
    //    phone that doesn't speak MANIFEST). Loads the epub — do it once.
    if (!pushedOpen_) {
      pushedOpen_ = true;
      KOReaderProgress kp;
      std::string th;
      if (BleProgress::getLocal(kp, th, /*deviceId=*/"x4")) {
        ble.sendProgress(kp.document, th, kp.progress, kp.percentage, kp.timestamp);
        if (!th.empty()) pushedKeys_.push_back(th);
        booksSent_++;
      }
      changed = true;
    }

    // 2. Send our MANIFEST; resend until the phone replies with its own.
    const bool manifestDue =
        !sentManifest_ || (!gotPhoneManifest_ && millis() - lastManifestMs_ > kResendMs);
    if (manifestDue) {
      ble.sendManifest(BleProgress::buildLocalManifest(kMaxManifest), /*more=*/false);
      sentManifest_ = true;
      lastManifestMs_ = millis();
      lastActivityMs_ = millis();
    }

    // 3. Drain inbound messages (MANIFEST / PROGRESS / WANT).
    proto::ParsedMessage m;
    while (ble.takeReceivedMessage(m)) {
      lastActivityMs_ = millis();
      if (m.type == proto::kTypeManifest) {
        gotPhoneManifest_ = true;
        // Push every local book we're strictly newer on (or the phone lacks).
        const auto mine = BleProgress::buildLocalManifest(kMaxManifest);
        for (const auto& lb : mine) {
          if (lb.updatedAt <= 0) continue;  // nothing clocked to offer
          int64_t phoneTs = -1;
          for (const auto& pb : m.books) {
            if (pb.titleHash == lb.titleHash) {
              phoneTs = pb.updatedAt;
              break;
            }
          }
          if (lb.updatedAt > phoneTs) pushProgressFor(lb.titleHash);
        }
        changed = true;
      } else if (m.type == proto::kTypeProgress) {
        KOReaderProgress kp;
        kp.document = m.document;
        kp.progress = m.xpointer;
        kp.percentage = m.percentage;
        kp.timestamp = m.updatedAt;
        if (BleProgress::applyRemote(kp, m.titleHash, renderer)) booksApplied_++;
        changed = true;
      } else if (m.type == proto::kTypeWant) {
        for (const auto& k : m.wantKeys) pushProgressFor(k);
        changed = true;
      }
    }
  }

  if (changed) {
    char b[64];
    std::snprintf(b, sizeof(b), "sent %d  applied %d", booksSent_, booksApplied_);
    {
      RenderLock lock(*this);
      state_ = next;
      statusSummary_ = b;
    }
    requestUpdate();
  }
}

void BleSyncTestActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, metrics.topPadding, screen.width, metrics.headerHeight},
                 "BLE Sync", nullptr);

  const char* status = state_ == State::Connected ? "Phone connected - syncing"
                                                  : "Broadcasting - open the app and tap Pair";
  const int lh = renderer.getLineHeight(UI_10_FONT_ID);
  const int cy = screen.y + screen.height / 4;
  renderer.drawCenteredText(UI_10_FONT_ID, cy, status, true);

  if (!statusSummary_.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, cy + lh * 2, statusSummary_.c_str(), true);
  }

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
