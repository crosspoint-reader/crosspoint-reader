#include "AppsActivity.h"

#include <Arduino.h>
#include <EInkDisplay.h>
#include <GfxRenderer.h>
#include <InputManager.h>
#include <MappedInputManager.h>
#include <SDCardManager.h>
#include <fontIds.h>

AppsActivity::AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ExitCallback exitCallback)
    : Activity("Apps", renderer, mappedInput),
      renderer_(renderer),
      mappedInput_(mappedInput),
      exitCallback_(exitCallback),
      selectedIndex_(0),
      needsUpdate_(true),
      isFlashing_(false),
      flashProgress_(0),
      flashingStarted_(false) {}

void AppsActivity::onEnter() {
  Activity::onEnter();
  scanApps();
  needsUpdate_ = true;
}

void AppsActivity::onExit() { Activity::onExit(); }

void AppsActivity::loop() {
  if (isFlashing_) {
    return;
  }

  if (mappedInput_.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput_.wasPressed(MappedInputManager::Button::Left)) {
    if (selectedIndex_ > 0) {
      selectedIndex_--;
      needsUpdate_ = true;
    }
  } else if (mappedInput_.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput_.wasPressed(MappedInputManager::Button::Right)) {
    if (selectedIndex_ < static_cast<int>(appList_.size()) - 1) {
      selectedIndex_++;
      needsUpdate_ = true;
    }
  } else if (mappedInput_.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!appList_.empty()) {
      launchApp();
    }
  } else if (mappedInput_.wasReleased(MappedInputManager::Button::Back)) {
    if (exitCallback_) {
      exitCallback_();
    }
  }

  if (needsUpdate_) {
    render();
    needsUpdate_ = false;
  }
}

void AppsActivity::scanApps() {
  appList_.clear();
  selectedIndex_ = 0;

  CrossPoint::AppLoader loader;
  appList_ = loader.scanApps();

  Serial.printf("[%lu] [AppsActivity] Found %d apps\n", millis(), appList_.size());
}

void AppsActivity::launchApp() {
  if (selectedIndex_ >= static_cast<int>(appList_.size())) {
    return;
  }

  const auto& app = appList_[selectedIndex_];

  Serial.printf("[%lu] [AppsActivity] Launching app: %s\n", millis(), app.manifest.name.c_str());

  isFlashing_ = true;
  flashProgress_ = 0;
  flashingStarted_ = false;
  needsUpdate_ = true;

  // Show progress UI immediately
  renderProgress();

  CrossPoint::AppLoader loader;
  
  // Build path to app binary (assumes app.bin in app directory)
  String binPath = app.path + "/app.bin";

  const String appId = app.path.substring(app.path.lastIndexOf('/') + 1);

  const bool success = loader.bootApp(binPath, appId, [this](size_t written, size_t total) {
    if (total == 0) {
      return;
    }

    const int nextProgress = static_cast<int>((written * 100) / total);
    if (!flashingStarted_) {
      flashingStarted_ = true;
    }

    if (nextProgress != flashProgress_) {
      flashProgress_ = nextProgress;
      needsUpdate_ = true;
      renderProgress();
    }
  });

  if (!success) {
    Serial.printf("[%lu] [AppsActivity] Flash failed\n", millis());
    isFlashing_ = false;
    needsUpdate_ = true;
  }
}

void AppsActivity::render() {
  renderer_.clearScreen();

  const int pageWidth = renderer_.getScreenWidth();
  const int pageHeight = renderer_.getScreenHeight();

  // Title
  renderer_.drawCenteredText(UI_12_FONT_ID, 30, "Apps");

  if (appList_.empty()) {
    renderer_.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "No apps found");
    renderer_.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, "Add apps to /.crosspoint/apps/");
  } else {
    // List apps
    const int startY = 70;
    const int lineHeight = 35;
    const int maxVisible = 10;

    int startIdx = 0;
    if (selectedIndex_ >= maxVisible) {
      startIdx = selectedIndex_ - maxVisible + 1;
    }

    for (int i = 0; i < maxVisible && (startIdx + i) < static_cast<int>(appList_.size()); i++) {
      int idx = startIdx + i;
      int y = startY + i * lineHeight;

      const auto& app = appList_[idx];
      char buf[128];
      snprintf(buf, sizeof(buf), "%s v%s", app.manifest.name.c_str(), app.manifest.version.c_str());

      if (idx == selectedIndex_) {
        // Highlight selected item
        int textWidth = renderer_.getTextWidth(UI_12_FONT_ID, buf);
        int x = (pageWidth - textWidth) / 2 - 10;
        renderer_.fillRect(x, y - 5, textWidth + 20, lineHeight - 5);
        // Draw white text on black highlight.
        renderer_.drawText(UI_12_FONT_ID, x + 10, y, buf, false);
      } else {
        renderer_.drawCenteredText(UI_10_FONT_ID, y, buf);
      }
    }

    // Scroll indicator
    if (appList_.size() > maxVisible) {
      char scrollInfo[32];
      snprintf(scrollInfo, sizeof(scrollInfo), "%d/%d", selectedIndex_ + 1, static_cast<int>(appList_.size()));
      renderer_.drawCenteredText(UI_10_FONT_ID, pageHeight - 80, scrollInfo);
    }
  }

  // Button hints
  const char* btn1 = "Back";
  const char* btn2 = !appList_.empty() ? "Install" : "";
  // Note: text is rotated 90° CW, so ">" appears as "^" and "<" appears as "v"
  const char* btn3 = ">";
  const char* btn4 = "<";

  auto labels = mappedInput_.mapLabels(btn1, btn2, btn3, btn4);
  renderer_.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer_.displayBuffer();
}

void AppsActivity::renderProgress() {
  renderer_.clearScreen();

  const int pageWidth = renderer_.getScreenWidth();
  const int pageHeight = renderer_.getScreenHeight();

  renderer_.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 40, "Flashing App...");

  // Progress bar
  const int barWidth = 300;
  const int barHeight = 30;
  const int barX = (pageWidth - barWidth) / 2;
  const int barY = pageHeight / 2;

  // Border
  renderer_.drawRect(barX, barY, barWidth, barHeight);

  // Fill
  int fillWidth = (flashProgress_ * barWidth) / 100;
  if (fillWidth > 0) {
    renderer_.fillRect(barX + 1, barY + 1, fillWidth - 2, barHeight - 2);
  }

  // Percentage text
  char percentStr[16];
  snprintf(percentStr, sizeof(percentStr), "%d%%", flashProgress_);
  renderer_.drawCenteredText(UI_12_FONT_ID, barY + barHeight + 20, percentStr);

  renderer_.displayBuffer();
}

void AppsActivity::renderBooting() {
  renderer_.clearScreen();

  const int pageHeight = renderer_.getScreenHeight();
  renderer_.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, "Booting...");
  renderer_.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, "Switching boot partition");
  renderer_.displayBuffer();
}

void AppsActivity::showProgress(size_t written, size_t total) {
  flashProgress_ = static_cast<int>((written * 100) / total);
  needsUpdate_ = true;
}
