#pragma once
#include "activities/Activity.h"
struct Metrics {
  int topPadding = 10, headerHeight = 100, verticalSpacing = 10, contentSidePadding = 20, progressBarHeight = 16;
};
struct UITheme {
  Metrics metrics;
  static UITheme& getInstance() {
    static UITheme t;
    return t;
  }
  const Metrics& getMetrics() const { return metrics; }
  void drawHeader(GfxRenderer&, Rect, const char*) const {}
};
#define GUI UITheme::getInstance()
