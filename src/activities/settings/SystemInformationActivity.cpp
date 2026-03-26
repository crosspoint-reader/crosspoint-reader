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
    snprintf(buf, sizeof(buf), "%.1f GiB", bytes / (1024.0 * 1024.0 * 1024.0));
  } else if (bytes >= 1024ULL * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MiB", bytes / (1024.0 * 1024.0));
  } else if (bytes >= 1024ULL) {
    snprintf(buf, sizeof(buf), "%.1f KiB", bytes / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  }
  return buf;
}

static std::string formatKiB(uint32_t bytes) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu KiB", (unsigned long)(bytes / 1024));
  return buf;
}

static std::string formatSdBytes(uint64_t bytes) {
  char buf[12];
  if (bytes >= 1000ULL * 1000 * 1000) {
    snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1000.0 * 1000.0 * 1000.0));
  } else if (bytes >= 1000ULL * 1000) {
    snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1000.0 * 1000.0));
  } else {
    snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1000.0);
  }
  return buf;
}

void SystemInformationActivity::onEnter() {
  Activity::onEnter();
  status_ = SystemStatus::collect();
  requestUpdate();
}

void SystemInformationActivity::onExit() { Activity::onExit(); }

void SystemInformationActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (!status_.has_value()) {
    status_ = SystemStatus::collect();
    requestUpdate();
  }
}

void SystemInformationActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SYSTEM_INFO));

  // Layout: labels aligned with contentSidePadding (matches SettingsActivity);
  // values at 2/5 of page width to leave more room than the old midpoint split.
  const int leftX = metrics.contentSidePadding + metrics.listItemInset;
  const int valueX = pageWidth * 2 / 5;
  const int maxValueWidth = pageWidth - valueX - metrics.contentSidePadding;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int startY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  auto drawRow = [&](int row, const char* label, const std::string& value) {
    const int y = startY + row * metrics.listRowHeight + metrics.listItemTextYOffset;
    renderer.drawText(UI_10_FONT_ID, leftX, y, label, true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, valueX, y, value.c_str());
  };

  if (!status_.has_value()) {
    // Stats not yet collected — show a placeholder so the screen updates immediately
    drawRow(0, tr(STR_FW_VERSION), renderer.truncatedText(UI_10_FONT_ID, CROSSPOINT_VERSION, maxValueWidth));
    drawRow(2, "", tr(STR_GATHERING_DATA));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto& status = *status_;

  drawRow(0, tr(STR_FW_VERSION), renderer.truncatedText(UI_10_FONT_ID, status.version, maxValueWidth));
  drawRow(1, tr(STR_FIRMWARE), formatKiB(status.sketchBytes) + " / " + formatKiB(status.sketchPartitionBytes));
  drawRow(2, tr(STR_CHIP), status.chipVersion + " (" + std::to_string(status.cpuFreqMHz) + " " + tr(STR_MHZ) + ")");
  drawRow(3, tr(STR_FLASH), formatBytes(status.flashBytes));
  constexpr uint32_t SRAM_TOTAL = 400u * 1024u;
  drawRow(4, tr(STR_FREE_RAM), formatBytes(SRAM_TOTAL));
  drawRow(5, tr(STR_MIN_FREE), formatBytes(SRAM_TOTAL - status.heapSizeBytes));
  drawRow(6, tr(STR_MAX_BLOCK),
          formatBytes(status.heapSizeBytes - status.freeHeapBytes) + " / " + formatBytes(status.heapSizeBytes));
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

  if (status.sdTotalBytes > 0) {
    drawRow(9, tr(STR_SD_CARD), formatSdBytes(status.sdUsedBytes) + " / " + formatSdBytes(status.sdTotalBytes));
  } else {
    drawRow(9, tr(STR_SD_CARD), tr(STR_NO_SD_CARD));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
