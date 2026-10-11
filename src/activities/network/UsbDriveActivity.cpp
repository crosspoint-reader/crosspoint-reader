#include "UsbDriveActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/TouchExitButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

void UsbDriveActivity::onEnter() {
  Activity::onEnter();

  // Paint the "preparing" screen and WAIT for it. Everything after this call
  // runs with the filesystem unmounted, and the e-ink refresh itself needs no
  // storage — but the user needs the eject instruction on screen before the
  // card disappears, not after.
  requestUpdateAndWait();

  // Mute LOG_* on the wire for the session. Starting MSC hands the shared USB
  // PHY to OTG, which leaves the USB Serial/JTAG peripheral behind HWCDC dead —
  // and on the transports that write unconditionally (FREEINK_LOG_TRANSPORT_USB_CDC_WRITE,
  // the T5S3) every log line would then block for the TX timeout against a
  // peripheral that can never drain. Logs still reach the RTC ring buffer, which
  // is where they are read from after the exit reboot anyway.
  setSerialWireMuted(true);

  if (!Storage.beginUsbDrive()) {
    LOG_ERR("USB", "Unable to start USB Drive");
    preparing = false;
    startFailed = true;
    state = UsbDriveState::IoError;
    startFailureStartedAt = millis();
    requestUpdate();
    return;
  }

  preparing = false;
  state = UsbDriveState::WaitingForHost;
  hostWaitStartedAt = millis();
  requestUpdate();
}

void UsbDriveActivity::onExit() {
  // restartToHome() already ended the session before rebooting; ending it twice
  // is harmless but pointless, and this path is also reached on a teardown that
  // is not ours (so leave the reboot to whoever asked for it).
  if (!restartRequested) Storage.endUsbDrive();
  setSerialWireMuted(false);
  Activity::onExit();
}

void UsbDriveActivity::loop() {
  if (!startFailed) {
    const UsbDriveState nextState = Storage.usbDriveState();
    if (nextState != state) {
      state = nextState;
      requestUpdate();
    }
  }

  if (state == UsbDriveState::WaitingForHost && millis() - hostWaitStartedAt >= HOST_WAIT_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive host wait timed out");
    restartToHome();
    return;
  }

  if (startFailed && millis() - startFailureStartedAt >= START_FAILURE_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive startup failure timed out");
    restartToHome();
    return;
  }

  // A sector read or write failed. The host's view of the volume is no longer
  // trustworthy, so drop the link rather than let it keep writing — a soft
  // disconnect first (the host sees the device leave and can flush its own
  // state), and a reboot if the host does not let go.
  if (!startFailed && isIoError()) {
    if (!forcedDisconnectRequested) {
      forcedDisconnectRequested = true;
      forcedDisconnectRequestedAt = millis();
      LOG_ERR("USB", "USB Drive I/O error; disconnecting host");
      if (!Storage.disconnectUsbDriveHost()) {
        LOG_ERR("USB", "Unable to request USB Drive host disconnect");
      }
    } else if (millis() - forcedDisconnectRequestedAt >= FORCED_DISCONNECT_TIMEOUT_MS) {
      LOG_ERR("USB", "USB Drive host disconnect timed out; forcing restart");
      restartToHome();
    }
    return;
  }

  // The cable may be long gone without TinyUSB noticing — see cableLooksGone().
  // Only meaningful once a host has actually been seen; before that the
  // host-wait timeout owns the exit, and on a battery-powered reader sitting
  // unplugged at the "connect me" screen VBUS is legitimately absent.
  if (state == UsbDriveState::Connected && cableLooksGone()) {
    restartToHome();
    return;
  }

  // Buttons are only live while nothing is mounted: leaving under a host that
  // still has the volume open is what corrupts cards, so once it is Connected
  // the way out is to eject or unplug.
  const bool canExitWithInput = state == UsbDriveState::WaitingForHost || startFailed;
  if (canExitWithInput && (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Power) || mappedInput.wasHomeGesture() ||
                           TouchExitButton::tapped(mappedInput))) {
    restartToHome();
    return;
  }

  if (state == UsbDriveState::Ejected || state == UsbDriveState::Disconnected || state == UsbDriveState::Unsupported) {
    restartToHome();
  }
}

bool UsbDriveActivity::cableLooksGone() {
  const unsigned long now = millis();

  // 1. Physical VBUS, where the board can read it. Throttled: I2C, not free.
  if (now - lastPowerPollAt >= POWER_POLL_INTERVAL_MS) {
    lastPowerPollAt = now;
    bool known = false;
    const bool present = Storage.usbDriveExternalPower(known);
    if (!known) {
      // The board cannot see the input rail. Stay silent rather than let a
      // permanent "false" masquerade as a permanent unplug.
      powerAbsentSince = 0;
    } else if (present) {
      powerAbsentSince = 0;
    } else if (powerAbsentSince == 0) {
      powerAbsentSince = now;
    }
  }
  if (powerAbsentSince != 0 && now - powerAbsentSince >= POWER_GONE_CONFIRM_MS) {
    LOG_INF("USB", "USB Drive: external power gone; ending session");
    return true;
  }

  // 2. Bus suspend, for boards with no VBUS reading. Ambiguous, so it needs to
  //    persist far longer before it is believed.
  if (Storage.usbDriveHostSuspended()) {
    if (suspendedSince == 0) suspendedSince = now;
    if (now - suspendedSince >= SUSPEND_GONE_CONFIRM_MS) {
      LOG_INF("USB", "USB Drive: bus suspended for %lums; assuming the cable is gone",
              static_cast<unsigned long>(now - suspendedSince));
      return true;
    }
  } else {
    suspendedSince = 0;
  }

  return false;
}

void UsbDriveActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_USB_DRIVE));

  // What the screen says for each state. `message` is the headline, the details
  // are the standing instructions underneath it.
  const char* message = nullptr;
  const char* detail = nullptr;
  const char* secondaryDetail = nullptr;
  if (preparing) {
    message = tr(STR_USB_DRIVE_PREPARING);
    detail = tr(STR_USB_DRIVE_EJECT_HINT);
  } else {
    switch (state) {
      case UsbDriveState::WaitingForHost:
        message = tr(STR_USB_DRIVE_WAITING);
        break;
      case UsbDriveState::Connected:
        message = tr(STR_USB_DRIVE_CONNECTED);
        detail = tr(STR_USB_DRIVE_CONNECT_DELAY);
        secondaryDetail = tr(STR_USB_DRIVE_EJECT_HINT);
        break;
      case UsbDriveState::IoError:
        message = startFailed ? tr(STR_USB_DRIVE_START_ERROR) : tr(STR_USB_DRIVE_ERROR);
        break;
      case UsbDriveState::Ejected:
      case UsbDriveState::Disconnected:
      case UsbDriveState::Unsupported:
        // loop() is already rebooting; painting a farewell screen would only
        // spend a blocking refresh on a frame nobody sees.
        renderer.displayBuffer();
        return;
    }
  }

  const int x = contentRect.x + metrics.contentSidePadding;
  const int maxWidth = contentRect.width - 2 * metrics.contentSidePadding;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3;

  auto drawWrapped = [&](const char* text, int maxLines, EpdFontFamily::Style style) {
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, text, maxWidth, maxLines, style)) {
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str(), true, style);
      y += lineH;
    }
  };

  drawWrapped(message, 2, EpdFontFamily::BOLD);
  if (detail) {
    y += metrics.verticalSpacing * 2;
    drawWrapped(detail, 3, EpdFontFamily::REGULAR);
  }
  if (secondaryDetail) {
    y += metrics.verticalSpacing * 2;
    drawWrapped(secondaryDetail, 3, EpdFontFamily::REGULAR);
  }

  // Only offer Back when it actually works — see the gate in loop().
  if (state == UsbDriveState::WaitingForHost || startFailed) {
    TouchExitButton::draw(renderer, mappedInput, x, y + metrics.verticalSpacing * 3, maxWidth);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    TouchExitButton::forget();
  }

  renderer.displayBuffer();
}

void UsbDriveActivity::restartToHome() {
  if (restartRequested) return;
  restartRequested = true;
  Storage.endUsbDrive();
  delay(20);  // let TinyUSB finish the teardown before the PHY handoff
  // The PHY is about to go back to Serial/JTAG and the reboot re-opens the log,
  // so stop muting here — restartToHomeAfterStorageHandoff() does not return
  // through onExit().
  setSerialWireMuted(false);
  restartToHomeAfterStorageHandoff();
}
