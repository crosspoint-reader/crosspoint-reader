#pragma once

#include <algorithm>
#include <GfxRenderer.h>
#include <I18n.h>
#include <string>

inline StrId getRssiQualityStrId(int rssi) {
  if (rssi <= -90) {
    return StrId::STR_SIGNAL_QUALITY_POOR;
  }
  if (rssi <= -75) {
    return StrId::STR_SIGNAL_QUALITY_WEAK;
  }
  if (rssi <= -60) {
    return StrId::STR_SIGNAL_QUALITY_GOOD;
  }
  return StrId::STR_SIGNAL_QUALITY_EXCELLENT;
}

inline void drawWifiSignalStrength(const GfxRenderer& renderer, int x, int y, int width, int height, int rssi) {
  const int barCount = 4;
  const int gap = 6;
  const int barWidth = std::min(10, (width - (barCount - 1) * gap) / barCount);
  const int totalWidth = barCount * barWidth + (barCount - 1) * gap;
  const int startX = x + (width - totalWidth) / 2;
  const int maxBarHeight = height - 8;
  const int bars = rssi == 0 ? 0 : (rssi <= -90 ? 1 : (rssi <= -75 ? 2 : (rssi <= -60 ? 3 : 4)));
  const int baseY = y + height - 2;
  for (int i = 0; i < barCount; ++i) {
    const int barHeight = ((i + 1) * maxBarHeight) / barCount;
    const int barX = startX + i * (barWidth + gap);
    const int barY = baseY - barHeight;
    renderer.drawRect(barX, barY, barWidth, barHeight, true);
    if (i < bars) {
      renderer.fillRect(barX + 1, barY + 1, barWidth - 2, barHeight - 2);
    }
  }
}

inline std::string rssiLabel(int rssi) {
  if (rssi == 0) {
    return std::string(tr(STR_NO_SIGNAL));
  }
  const char* quality = I18N.get(getRssiQualityStrId(rssi));
  return std::string(tr(STR_RSSI)) + ": " + std::to_string(rssi) + " dBm (" + quality + ")";
}
