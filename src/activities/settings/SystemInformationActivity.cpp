#include "SystemInformationActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "SystemStatus.h"
#include "components/UITheme.h"
#include "fontIds.h"

static std::string formatBytes(uint64_t bytes) {
  char buf[16];
  if (bytes >= 1024ULL * 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
  } else if (bytes >= 1024ULL * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
  } else if (bytes >= 1024ULL) {
    snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  }
  return buf;
}

void SystemInformationActivity::onEnter() {
  Activity::onEnter();
  status_.reset();
  requestUpdate();
}

void SystemInformationActivity::onExit() { Activity::onExit(); }

void SystemInformationActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  // Collect status (includes the potentially slow SD FAT walk) outside of render()
  // so the screen is shown with a "Reading..." placeholder first.
  if (!status_.has_value()) {
    status_ = SystemStatus::collect();
    requestUpdate();
  }
}

void SystemInformationActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SYSTEM_INFO),
                 CROSSPOINT_VERSION);

  // Layout: label on the left, value right of the midpoint
  const int leftX = metrics.verticalSpacing * 3;
  const int valueX = pageWidth / 2;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int startY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3;

  auto drawRow = [&](int row, const char* label, const std::string& value) {
    const int y = startY + row * (lineH + metrics.verticalSpacing);
    renderer.drawText(UI_10_FONT_ID, leftX, y, label, true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, valueX, y, value.c_str());
  };

  if (!status_.has_value()) {
    // Stats not yet collected — show a placeholder so the screen updates immediately
    drawRow(0, "Version", CROSSPOINT_VERSION);
    drawRow(3, "SD card", "Reading...");
    renderer.displayBuffer();
    return;
  }

  const auto& status = *status_;

  // Device
  drawRow(0, "Version", status.version);
  drawRow(1, "Free heap", std::to_string(status.freeHeapBytes / 1024) + " KB");

  const uint32_t h = status.uptimeSeconds / 3600;
  const uint32_t m = (status.uptimeSeconds % 3600) / 60;
  const uint32_t s = status.uptimeSeconds % 60;
  char uptimeBuf[16];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%uh %02um %02us", h, m, s);
  drawRow(2, "Uptime", uptimeBuf);

  // SD card
  const std::string sdUsed = formatBytes(status.sdUsedBytes) + " / " + formatBytes(status.sdTotalBytes);
  drawRow(3, "SD card", status.sdTotalBytes > 0 ? sdUsed : "N/A");

  // WiFi (shown as-is; "Off" when not connected)
  std::string wifiLabel = status.wifiMode;
  if (status.rssi != 0) {
    wifiLabel += " (" + std::to_string(status.rssi) + " dBm)";
  }
  drawRow(4, "WiFi", wifiLabel);
  if (status.wifiMode != "Off") {
    drawRow(5, "IP address", status.ip);
    drawRow(6, "MAC address", status.macAddress);
  } else {
    drawRow(5, "MAC address", status.macAddress);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
