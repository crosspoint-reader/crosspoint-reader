#include "ServerSession.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalSystem.h>  // feedWatchdog()
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "SilentRestart.h"
#include "activities/NetworkMemoryTrim.h"
#include "components/TouchExitButton.h"

void ServerSession::prepareRadio() {
  // A loaded SD font (Literata etc.) holds ~24-60KB of kern/ligature/glyph data that is
  // completely unused during a web server session. The device restarts when the session ends
  // (end()), so it is reloaded fresh on the next boot anyway. Then the secondary framebuffer and
  // the glyph cache, as for every network activity; WifiSelectionActivity trims too, and the
  // hotspot never goes through it.
  LOG_DBG(tag, "Free heap before trim: %d bytes", ESP.getFreeHeap());
  sdFontSystem.unload(renderer);
  trimMemoryForNetworkSession(renderer, tag);
  LOG_DBG(tag, "Free heap after trim: %d bytes", ESP.getFreeHeap());
  powerManager.ensureFullSpeedForRadio();  // WiFi needs >=80 MHz; see HalPowerManager
}

bool ServerSession::start(const bool hotspot) {
  if (MDNS.begin(HOSTNAME)) {
    LOG_DBG(tag, "mDNS started: http://%s.local/", HOSTNAME);
  } else {
    LOG_DBG(tag, "WARNING: mDNS failed to start");
  }

  server = makeUniqueNoThrow<CrossPointWebServer>();
  if (!server) {
    LOG_ERR(tag, "OOM allocating web server");
    return false;
  }
  // Ported from crosspoint-reader PR #3732 ("fix: let Back leave File Transfer during a slow HTTP
  // upload", Tuan Q. Nguyen / @martinqnguyen). Theirs pumps input here (update(), plus a second
  // sample 6 ms later to clear the SDK debounce); ours only reads what the button sampler task has
  // latched. That task outranks the loop task and keeps its 10 ms cadence while the loop is stuck
  // in an upload, so a press made between two chunks is already waiting when the next one lands.
  //
  // Runs on the loop task inside handleClient(), holding no RenderLock, storage or I2C lock, and
  // takes none: a snapshot read and a short critical section. Neither consumes the press, unlike
  // update(), which would also drain the other buttons and the touch event from under the loop.
  // Back can be in either place: still in the sampler's accumulator, or drained into this tick's
  // snapshot by the main loop just before the upload began.
  leaveRequested = false;
  server->setUploadCancelCheck([this] {
    leaveRequested = leaveRequested || mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                     mappedInput.hasPendingPress(MappedInputManager::Button::Back);
    return leaveRequested;
  });
  server->begin();
  if (!server->isRunning()) {
    LOG_ERR(tag, "ERROR: Failed to start web server!");
    server.reset();
    return false;
  }
  LOG_DBG(tag, "Web server started successfully");
  if (!hotspot) linkWatch.start();
  return true;
}

bool ServerSession::serve(const int rounds) {
  // Read without update(): that would replace this tick's input snapshot, touch included, so a tap
  // on the hint strip or the Exit button was gone before anything looked at it. hasPendingPress()
  // sees a key pressed since the tick began without draining anything, as the upload cancel check
  // does.
  const auto backPressed = [this] {
    return mappedInput.wasPressed(MappedInputManager::Button::Back) ||
           mappedInput.hasPendingPress(MappedInputManager::Button::Back);
  };
  if (TouchExitButton::tapped(mappedInput) || backPressed()) return false;
  if (!running()) return true;

  if (lastServeEnd > 0 && millis() - lastServeEnd > 100) {
    LOG_DBG(tag, "WARNING: %lu ms gap since last handleClient", millis() - lastServeEnd);
  }
  // Before processing: HTTP header parsing can be slow
  HalSystem::feedWatchdog();
  for (int i = 0; i < rounds && server->isRunning(); i++) {
    server->handleClient();
    // The upload cancel check saw Back and has already dropped the upload.
    if (leaveRequested) return false;
    if ((i & 0x1F) == 0x1F) HalSystem::feedWatchdog();
    if ((i & 0x3F) == 0x3F) {
      yield();
      if (backPressed()) return false;
    }
  }
  lastServeEnd = millis();
  return true;
}

void ServerSession::end() {
  linkWatch.stop();  // before the disconnect below, a drop of our own making
  server.reset();    // its destructor stops it
  MDNS.end();

  // Nothing to undo when WiFi never came up (the user backed out of the mode or network choice).
  const wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_MODE_NULL) return;
  if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
    WiFi.softAPdisconnect(true);
  } else {
    WiFi.disconnect(false);
  }
  delay(30);
  silentRestart();
}
