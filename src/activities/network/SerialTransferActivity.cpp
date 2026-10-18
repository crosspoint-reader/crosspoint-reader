#include "SerialTransferActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "SerialTestApi.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Below this free-heap level at entry we release the secondary framebuffer to
// give the transfer (16 KB coalesce buffer + SD/FAT structures) headroom.
// Threshold is a conservative guess pending on-device validation.
constexpr uint32_t kLowHeapTrimThreshold = 60 * 1024;

// Process at most this many protocol messages per loop() so a flood of small
// commands can't starve the Back-button check.
constexpr int kMaxMessagesPerLoop = 64;

#if SERIAL_TEST_API
// A "CMD:" line arrives in one write from a script; one still unfinished after this long is noise.
constexpr unsigned long kTextCommandTimeoutMs = 2000;
#endif

// Raise HWCDC's TX timeout for the session from its 100ms default. At 100ms, a
// brief ring-drain timing race at a chunk boundary flips the link to
// "disconnected" and silently drops TX, stalling a download. 1000ms gives the
// drain ISR ample slack while staying well under the 5s task watchdog (logs are
// muted during the session, so nothing else blocks on it).
constexpr uint32_t kSerialTxTimeoutMs = 1000;
}  // namespace

void SerialTransferActivity::statusThunk(void* ctx, const char* msg) {
  static_cast<SerialTransferActivity*>(ctx)->onStatus(msg);
}

void SerialTransferActivity::onStatus(const char* msg) {
  // Only record the message here — do NOT paint. This is called from inside the
  // protocol (mid-transfer); a blocking e-ink refresh in that path delays
  // protocol bytes and intermittently corrupted transfers. loop() paints the
  // latest message between protocol messages, when no SD/serial is in flight.
  statusMessage_ = msg ? msg : "";
  statusDirty_ = true;
}

void SerialTransferActivity::onEnter() {
  Activity::onEnter();

  // Mute LOG_* on the wire for the whole session so no stray log byte from a
  // background task can corrupt the binary protocol stream. Logs still reach the
  // RTC ring buffer (getLastLogs). (On a boot-into-transfer, setup() already
  // muted; this keeps it muted.)
  setSerialWireMuted(true);
  logSerial.setTxTimeoutMs(kSerialTxTimeoutMs);  // see constant; restored in onExit

  // Arm the boot target: opening the port resets the C3 (unavoidable hardware
  // reset), so without this the device would land on Home and the host couldn't
  // transfer. Armed here (no reboot) so the reset routes setup() straight back
  // into this activity. Re-armed on every entry (setup() clears the flag).
  armSerialTransferReboot();

  device_.setStatusCallback(&SerialTransferActivity::statusThunk, this);
  statusMessage_ = "Ready - waiting for host";
  requestUpdateAndWait();  // paint the idle screen before any trim

  // Under memory pressure, free the secondary framebuffer (the primary stays so
  // we can still paint status updates). We silentRestart() on exit to rebuild it.
  if (ESP.getFreeHeap() < kLowHeapTrimThreshold) {
    if (auto* cache = renderer.getFontCacheManager()) cache->clearCache();
    // Seed RED RAM with the displayed frame before the release, as the X4's single-buffer fast
    // diff requires (no-op on X3); the lock keeps a status repaint off the bus meanwhile.
    RenderLock lock;
    if (renderer.hasSecondaryBuffer()) renderer.syncRedRamFromFrameBuffer();
    if (renderer.hasSecondaryBuffer() && renderer.releaseSecondaryBuffer()) {
      renderer.setSingleBufferFastDiff(true);
      LOG_DBG("SXF", "Released secondary framebuffer under low heap (free=%u)",
              static_cast<unsigned>(ESP.getFreeHeap()));
    }
  }
}

void SerialTransferActivity::onExit() {
  device_.setStatusCallback(nullptr, nullptr);
  setSerialWireMuted(false);
  // Restore the firmware's baseline, NOT HWCDC's 100 ms default: on transports
  // that write unconditionally, 100 ms per line blocks long enough to trip the
  // interrupt watchdog. See the setTxTimeoutMs call in main.cpp setup().
  logSerial.setTxTimeoutMs(1);
  // Clear the arm first, in case silentRestart() below no-ops (e.g. a deep sleep
  // is already in progress), so we don't wake back into this activity.
  disarmSerialTransferReboot();
  Activity::onExit();

  // Always reboot to Home on exit. The boot-into-transfer path skips SD-font
  // discovery (and may have freed the secondary framebuffer), so a clean boot is
  // the simplest way to restore full state for Home/reader. Matches the web
  // server activity. silentRestart() targets Home and does not return.
  silentRestart();
}

void SerialTransferActivity::loop() {
  // Check Back first so the session can be left between protocol frames.
  // finish() (not onGoHome()) returns control to the File Transfer picker that
  // launched us via startActivityForResult, matching the Calibre branch.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Drive the protocol. Each poll() handles at most one complete message and
  // returns false when the leading bytes aren't a magic or fewer than 4 bytes
  // are buffered. A "CMD:" there is a test command, read to its line end;
  // anything else slides the probe forward a byte to resync.
  for (int i = 0; i < kMaxMessagesPerLoop; ++i) {
#if SERIAL_TEST_API
    if (readingTextCommand_) {
      if (continueTextCommand()) continue;
      break;  // the rest of the line has not arrived yet
    }
#endif
    if (device_.poll()) continue;
    if (device_.available() >= 4) {
#if SERIAL_TEST_API
      if (device_.textCommandAhead()) {
        readingTextCommand_ = true;
        textCommandStartMs_ = millis();
        continue;
      }
#endif
      device_.dropByte();
      continue;
    }
    break;  // nothing actionable this iteration
  }

  // Paint the latest status now that the protocol is idle this iteration — no SD
  // or serial in flight, so the blocking e-ink refresh is SPI-safe and can't
  // delay a transfer.
  if (statusDirty_) {
    statusDirty_ = false;
    requestUpdateAndWait();
  }
}

#if SERIAL_TEST_API
bool SerialTransferActivity::continueTextCommand() {
  while (device_.available() > 0) {
    const int b = device_.readByte();
    if (b < 0) break;
    if (b == '\n' || b == '\r') {
      readingTextCommand_ = false;
      // One that overflowed the buffer is dropped: a cut command could act differently.
      if (textLineLength_ < kTextLineBytes) SerialTestApi::handleLine(std::string_view(textLine_, textLineLength_));
      textLineLength_ = 0;
      return true;
    }
    if (textLineLength_ < kTextLineBytes) textLine_[textLineLength_] = static_cast<char>(b);
    if (textLineLength_ <= kTextLineBytes) textLineLength_++;
  }
  if (millis() - textCommandStartMs_ >= kTextCommandTimeoutMs) {
    LOG_DBG("SXF", "Dropped an unfinished CMD: line");
    textLineLength_ = 0;
    readingTextCommand_ = false;
    return true;
  }
  return false;
}
#endif  // SERIAL_TEST_API

void SerialTransferActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_USB_TRANSFER));

  const int x = contentRect.x + metrics.contentSidePadding;
  const int maxWidth = contentRect.width - 2 * metrics.contentSidePadding;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  // Word-wrap both texts so long file names / hints don't overflow the screen.
  auto drawWrapped = [&](const char* text, int maxLines, EpdFontFamily::Style style) {
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, text, maxWidth, maxLines, style)) {
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str(), true, style);
      y += lineH;
    }
  };

  // Instruction chrome (translated). Generous line budget so a longer hint
  // isn't truncated; it sits at the top with room above the status line.
  drawWrapped(tr(STR_USB_TRANSFER_HINT), 5, EpdFontFamily::REGULAR);
  y += metrics.verticalSpacing * 2;

  // Live status / diagnostic line (dynamic, includes file names; not translated).
  drawWrapped(statusMessage_.c_str(), 3, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
