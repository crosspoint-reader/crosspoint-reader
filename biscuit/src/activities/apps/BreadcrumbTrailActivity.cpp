#include "BreadcrumbTrailActivity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ----------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------

void BreadcrumbTrailActivity::onEnter() {
  Activity::onEnter();
  RADIO.ensureWifi();
  state = MENU;
  menuIndex = 0;
  trail.clear();
  retraceIndex = 0;
  matchScore = 0.0f;
  lastCrumb = 0;
  trailFiles.clear();
  trailListIndex = 0;
  requestUpdate();
}

void BreadcrumbTrailActivity::onExit() {
  Activity::onExit();
  WiFi.scanDelete();
  RADIO.shutdown();
}

// ----------------------------------------------------------------
// WiFi helpers
// ----------------------------------------------------------------

void BreadcrumbTrailActivity::doScan(Crumb& out) {
  out.apCount = 0;
  out.timestamp = millis();
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) return;

  // Collect all APs with RSSI and pick top 5
  static ApSig buf[32];
  int total = (n > 32) ? 32 : n;
  for (int i = 0; i < total; i++) {
    memcpy(buf[i].bssid, WiFi.BSSID(i), 6);
    buf[i].rssi = (int8_t)WiFi.RSSI(i);
  }
  // Bubble-sort descending by RSSI (small count, stack only)
  for (int i = 0; i < total - 1; i++)
    for (int j = i + 1; j < total; j++)
      if (buf[j].rssi > buf[i].rssi) {
        ApSig tmp = buf[i];
        buf[i] = buf[j];
        buf[j] = tmp;
      }

  out.apCount = (total < 5) ? total : 5;
  for (int i = 0; i < out.apCount; i++) out.aps[i] = buf[i];
  WiFi.scanDelete();
}

float BreadcrumbTrailActivity::jaccardScore(const Crumb& crumb, const ApSig* current, int currentCount) const {
  // Count matching BSSIDs
  int matches = 0;
  for (int i = 0; i < crumb.apCount; i++)
    for (int j = 0; j < currentCount; j++)
      if (memcmp(crumb.aps[i].bssid, current[j].bssid, 6) == 0) { matches++; break; }
  int unionCount = crumb.apCount + currentCount - matches;
  if (unionCount <= 0) return 0.0f;
  return (float)matches / (float)unionCount * 100.0f;
}

// ----------------------------------------------------------------
// Storage
// ----------------------------------------------------------------

void BreadcrumbTrailActivity::saveToDisk(const char* name) const {
  Storage.mkdir("/biscuit");
  Storage.mkdir(TRAILS_DIR);
  char path[80];
  snprintf(path, sizeof(path), "%s/%s.dat", TRAILS_DIR, name);
  auto f = Storage.open(path, O_WRITE | O_CREAT | O_TRUNC);
  if (!f) return;
  uint16_t count = (uint16_t)trail.size();
  f.write(reinterpret_cast<const uint8_t*>(&count), sizeof(count));
  f.write(reinterpret_cast<const uint8_t*>(trail.data()), sizeof(Crumb) * count);
  f.close();
}

bool BreadcrumbTrailActivity::loadFromDisk(const char* path) {
  trail.clear();
  auto f = Storage.open(path);
  if (!f) return false;
  uint16_t count = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count)) != sizeof(count)) { f.close(); return false; }
  if (count == 0 || count > MAX_CRUMBS) { f.close(); return false; }
  trail.resize(count);
  f.read(reinterpret_cast<uint8_t*>(trail.data()), sizeof(Crumb) * count);
  f.close();
  return true;
}

void BreadcrumbTrailActivity::loadTrailList() {
  trailFiles.clear();
  HalFile dir = Storage.open(TRAILS_DIR);
  if (!dir) return;
  HalFile entry;
  while ((entry = dir.openNextFile()) && trailFiles.size() < 32) {
    char nameBuf[64];
    entry.getName(nameBuf, sizeof(nameBuf));
    std::string n = nameBuf;
    if (!n.empty() && n[0] != '.') trailFiles.push_back(n);
    entry.close();
  }
  dir.close();
}

// ----------------------------------------------------------------
// Loop
// ----------------------------------------------------------------

void BreadcrumbTrailActivity::loop() {
  if (state == MENU) {
    static constexpr int MENU_COUNT = 3;
    buttonNavigator.onNext([this] { menuIndex = ButtonNavigator::nextIndex(menuIndex, MENU_COUNT); requestUpdate(); });
    buttonNavigator.onPrevious([this] { menuIndex = ButtonNavigator::previousIndex(menuIndex, MENU_COUNT); requestUpdate(); });
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { finish(); return; }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (menuIndex == 0) {
        trail.clear();
        lastCrumb = 0;
        state = RECORDING;
        requestUpdate();
      } else if (menuIndex == 1) {
        loadTrailList();
        trailListIndex = 0;
        state = TRAIL_LIST;
        requestUpdate();
      } else {
        finish();
      }
    }
    return;
  }

  if (state == RECORDING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // Stop recording, offer to save
      if (!trail.empty()) {
        promptSaveName();
      } else {
        state = MENU;
        menuIndex = 0;
        requestUpdate();
      }
      return;
    }
    // Auto-crumb every 30 seconds
    unsigned long now = millis();
    if (now - lastCrumb >= 30000 && (int)trail.size() < MAX_CRUMBS) {
      Crumb c{};
      doScan(c);
      trail.push_back(c);
      lastCrumb = now;
      requestUpdate();
    } else if (lastCrumb == 0) {
      lastCrumb = now;
    }
    return;
  }

  if (state == TRAIL_LIST) {
    buttonNavigator.onNext([this] { trailListIndex = ButtonNavigator::nextIndex(trailListIndex, (int)trailFiles.size()); requestUpdate(); });
    buttonNavigator.onPrevious([this] { trailListIndex = ButtonNavigator::previousIndex(trailListIndex, (int)trailFiles.size()); requestUpdate(); });
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { state = MENU; requestUpdate(); return; }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !trailFiles.empty()) {
      char path[80];
      snprintf(path, sizeof(path), "%s/%s", TRAILS_DIR, trailFiles[trailListIndex].c_str());
      if (loadFromDisk(path)) {
        retraceIndex = 0;
        matchScore = 0.0f;
        lastCrumb = 0;
        state = RETRACING;
        requestUpdate();
      }
    }
    return;
  }

  if (state == RETRACING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) { state = MENU; trail.clear(); requestUpdate(); return; }
    unsigned long now = millis();
    if (now - lastCrumb >= 5000) {
      lastCrumb = now;
      Crumb current{};
      doScan(current);
      // Find best matching crumb
      float best = 0.0f;
      int bestIdx = 0;
      for (int i = 0; i < (int)trail.size(); i++) {
        float s = jaccardScore(trail[i], current.aps, current.apCount);
        if (s > best) { best = s; bestIdx = i; }
      }
      matchScore = best;
      retraceIndex = bestIdx;
      requestUpdate();
    }
    return;
  }
}

void BreadcrumbTrailActivity::promptSaveName() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Trail Name", "trail", 32),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto& text = std::get<KeyboardResult>(result.data).text;
          if (!text.empty()) saveToDisk(text.c_str());
        }
        state = MENU;
        menuIndex = 0;
      });
}

// ----------------------------------------------------------------
// Render
// ----------------------------------------------------------------

void BreadcrumbTrailActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (state) {
    case MENU:      renderMenu();      break;
    case RECORDING: renderRecording(); break;
    case TRAIL_LIST: renderTrailList(); break;
    case RETRACING: renderRetracing(); break;
  }
  renderer.displayBuffer();
}

void BreadcrumbTrailActivity::renderMenu() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Breadcrumb Trail");
  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight;
  static const char* const OPTIONS[] = {"Record Trail", "Retrace Trail", "Back"};
  GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, 3, menuIndex,
               [](int i) -> std::string { return OPTIONS[i]; });
  const auto labels = mappedInput.mapLabels("Back", "Select", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BreadcrumbTrailActivity::renderRecording() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Recording Trail");

  int y = metrics.topPadding + metrics.headerHeight + 30;
  char buf[48];
  snprintf(buf, sizeof(buf), "Crumbs: %d / %d", (int)trail.size(), MAX_CRUMBS);
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf, true, EpdFontFamily::BOLD);
  y += 50;

  unsigned long elapsed = (millis() - lastCrumb) / 1000;
  unsigned long nextIn = (elapsed < 30) ? (30 - elapsed) : 0;
  snprintf(buf, sizeof(buf), "Next crumb in: %lus", nextIn);
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
  y += 40;

  unsigned long totalSecs = (lastCrumb > 0) ? (((unsigned long)trail.size() * 30000 + (millis() - lastCrumb)) / 1000) : 0;
  snprintf(buf, sizeof(buf), "Elapsed: %lum %02lus", totalSecs / 60, totalSecs % 60);
  renderer.drawCenteredText(SMALL_FONT_ID, y, buf);

  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - metrics.buttonHintsHeight - 25,
                             "Back = stop & save");
  const auto labels = mappedInput.mapLabels("Stop", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BreadcrumbTrailActivity::renderTrailList() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Select Trail");
  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight;
  if (trailFiles.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, listTop + listH / 2 - 10, "No trails saved.");
  } else {
    GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, (int)trailFiles.size(), trailListIndex,
                 [this](int i) -> std::string { return trailFiles[i]; });
  }
  const auto labels = mappedInput.mapLabels("Back", "Load", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BreadcrumbTrailActivity::renderRetracing() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Retracing Trail");

  int y = metrics.topPadding + metrics.headerHeight + 30;
  char buf[48];
  snprintf(buf, sizeof(buf), "Match: %.0f%%", matchScore);
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf, true, EpdFontFamily::BOLD);
  y += 55;

  snprintf(buf, sizeof(buf), "Crumb: %d / %d", retraceIndex + 1, (int)trail.size());
  renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
  y += 40;

  const char* status = (matchScore >= 40.0f) ? "ON TRACK" : "OFF TRACK";
  renderer.drawCenteredText(UI_12_FONT_ID, y, status, true, EpdFontFamily::BOLD);
  y += 45;

  unsigned long elapsedSec = (millis() - lastCrumb) / 1000;
  unsigned long nextIn = (elapsedSec < 5) ? (5 - elapsedSec) : 0;
  snprintf(buf, sizeof(buf), "Scan in: %lus", nextIn);
  renderer.drawCenteredText(SMALL_FONT_ID, y, buf);

  const auto labels = mappedInput.mapLabels("Stop", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  (void)pageHeight;
}
