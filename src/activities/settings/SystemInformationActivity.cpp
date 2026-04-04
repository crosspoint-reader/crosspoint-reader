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
  sdStatusReady_ = false;
  requestUpdate();
}

void SystemInformationActivity::onExit() { Activity::onExit(); }

void SystemInformationActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  // Collect fast fields first so this page appears immediately.
  if (!status_.has_value()) {
    status_ = SystemStatus::collectFast();
    requestUpdate();
    return;
  }

  // SD stats can be slower to compute on large cards.
  if (!sdStatusReady_) {
    SystemStatus::fillSdStatus(*status_);
    sdStatusReady_ = true;
    requestUpdate();
  }
}

void SystemInformationActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

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
    drawRow(0, tr(STR_FW_VERSION), CROSSPOINT_VERSION);
    drawRow(2, "", tr(STR_GATHERING_DATA));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto& status = *status_;

  drawRow(0, tr(STR_FW_VERSION), status.version);
  drawRow(1, tr(STR_CHIP), status.chipVersion);
  drawRow(2, tr(STR_CPU), std::to_string(status.cpuFreqMHz) + " " + tr(STR_MHZ));
  drawRow(3, tr(STR_FREE_RAM), formatBytes(status.freeHeapBytes));
  drawRow(4, tr(STR_MIN_FREE), formatBytes(status.minFreeHeapBytes));
  drawRow(5, tr(STR_MAX_BLOCK), formatBytes(status.maxAllocHeapBytes));
  drawRow(
      6, tr(STR_FLASH_USED),
      formatBytes(status.flashAppUsedBytes) + " / " + formatBytes(status.flashAppUsedBytes + status.flashAppFreeBytes));
  std::string batteryLabel = std::to_string(status.batteryPercent) + "%";
  if (status.charging) {
    batteryLabel += " (";
    batteryLabel += tr(STR_CHARGING);
    batteryLabel += ")";
  }
  drawRow(7, tr(STR_BATTERY), batteryLabel);

  const uint32_t h = status.uptimeSeconds / 3600;
  const uint32_t m = (status.uptimeSeconds % 3600) / 60;
  const uint32_t s = status.uptimeSeconds % 60;
  char uptimeBuf[16];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%uh %02um %02us", h, m, s);
  drawRow(8, tr(STR_UPTIME), uptimeBuf);

  if (!sdStatusReady_) {
    drawRow(9, tr(STR_SD_CARD), tr(STR_READING));
  } else if (status.sdTotalBytes > 0) {
    drawRow(9, tr(STR_SD_CARD), formatBytes(status.sdUsedBytes) + " / " + formatBytes(status.sdTotalBytes));
  } else {
    drawRow(9, tr(STR_SD_CARD), tr(STR_NOT_SET));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
