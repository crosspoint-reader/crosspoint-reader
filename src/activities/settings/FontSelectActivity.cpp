#include "FontSelectActivity.h"

#include <FontManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kBuiltinReaderFontCount = 3;
constexpr CrossPointSettings::FONT_FAMILY kBuiltinReaderFonts[kBuiltinReaderFontCount] = {
    CrossPointSettings::BOOKERLY, CrossPointSettings::NOTOSANS, CrossPointSettings::OPENDYSLEXIC};
constexpr StrId kBuiltinReaderFontLabels[kBuiltinReaderFontCount] = {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS,
                                                                     StrId::STR_OPEN_DYSLEXIC};
}  // namespace

void FontSelectActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Wait for parent activity's rendering to complete (screen refresh takes ~422ms)
  // Wait 500ms to be safe and avoid race conditions with parent activity
  vTaskDelay(500 / portTICK_PERIOD_MS);

  // Scan fonts
  FontMgr.scanFonts();

  if (mode == SelectMode::Reader) {
    totalItems = kBuiltinReaderFontCount + FontMgr.getFontCount();

    const int currentExternal = FontMgr.getSelectedIndex();
    if (currentExternal >= 0) {
      selectedIndex = kBuiltinReaderFontCount + currentExternal;
    } else {
      const int familyIndex = static_cast<int>(SETTINGS.fontFamily);
      selectedIndex = (familyIndex < kBuiltinReaderFontCount) ? familyIndex : 0;
    }
  } else {
    // Built-in UI font + external fonts
    totalItems = 1 + FontMgr.getFontCount();

    const int currentFont = FontMgr.getUiSelectedIndex();
    selectedIndex = (currentFont < 0) ? 0 : currentFont + 1;
  }

  // 同步渲染，不使用后台任务
  render();
}

void FontSelectActivity::onExit() {
  ActivityWithSubactivity::onExit();
  // 不需要清理任务和 mutex，因为我们不再使用它们
}

void FontSelectActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  bool needsRender = false;

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + totalItems - 1) % totalItems;
    needsRender = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % totalItems;
    needsRender = true;
  }

  // 同步渲染
  if (needsRender) {
    render();
  }
}

void FontSelectActivity::handleSelection() {
  LOG_DBG("FNT", "handleSelection: mode=%d, selectedIndex=%d", static_cast<int>(mode), selectedIndex);

  if (mode == SelectMode::Reader) {
    if (selectedIndex < kBuiltinReaderFontCount) {
      // Select built-in reader font
      LOG_DBG("FNT", "Selecting built-in reader font index %d", selectedIndex);
      FontMgr.selectFont(-1);
      SETTINGS.fontFamily = static_cast<uint8_t>(kBuiltinReaderFonts[selectedIndex]);
      SETTINGS.saveToFile();
    } else {
      // Select external reader font (skip if glyph too large)
      const int externalIndex = selectedIndex - kBuiltinReaderFontCount;
      const FontInfo* info = FontMgr.getFontInfo(externalIndex);
      if (info && !ExternalFont::canFitGlyph(info->width, info->height)) {
        LOG_DBG("FNT", "Font %s glyph too large (%dx%d), skipping", info->name, info->width, info->height);
        return;
      }
      LOG_DBG("FNT", "Selecting reader font index %d", externalIndex);
      FontMgr.selectFont(externalIndex);
    }
    renderer.setReaderFallbackFontId(SETTINGS.getBuiltInReaderFontId());
  } else {
    if (selectedIndex == 0) {
      // Select built-in UI font (disable external font)
      LOG_DBG("FNT", "Disabling UI font");
      FontMgr.selectUiFont(-1);
    } else {
      // Select external UI font (skip if glyph too large)
      const int externalIndex = selectedIndex - 1;
      const FontInfo* info = FontMgr.getFontInfo(externalIndex);
      if (info && !ExternalFont::canFitGlyph(info->width, info->height)) {
        LOG_DBG("FNT", "Font %s glyph too large (%dx%d), skipping", info->name, info->width, info->height);
        return;
      }
      LOG_DBG("FNT", "Selecting UI font index %d", externalIndex);
      FontMgr.selectUiFont(externalIndex);
    }
  }

  LOG_DBG("FNT", "After selection: readerIndex=%d, uiIndex=%d", FontMgr.getSelectedIndex(),
          FontMgr.getUiSelectedIndex());

  // Return to previous page
  onBack();
}

void FontSelectActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;

  // Title
  const char* title = (mode == SelectMode::Reader) ? tr(STR_EXT_READER_FONT) : tr(STR_EXT_UI_FONT);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding + hintGutterHeight, pageWidth, metrics.headerHeight}, title);

  // Current active font index (for the ON marker)
  int currentIndex = 0;
  if (mode == SelectMode::Reader) {
    const int currentExternal = FontMgr.getSelectedIndex();
    if (currentExternal >= 0) {
      currentIndex = kBuiltinReaderFontCount + currentExternal;
    } else {
      const int familyIndex = static_cast<int>(SETTINGS.fontFamily);
      currentIndex = (familyIndex < kBuiltinReaderFontCount) ? familyIndex : 0;
    }
  } else {
    const int currentFont = FontMgr.getUiSelectedIndex();
    currentIndex = (currentFont < 0) ? 0 : currentFont + 1;
  }

  // Font list
  const int contentTop = metrics.topPadding + hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalItems, selectedIndex,
      [this](int i) -> std::string {
        if (mode == SelectMode::Reader) {
          if (i < kBuiltinReaderFontCount) {
            return std::string(I18N.get(kBuiltinReaderFontLabels[i]));
          }
          const FontInfo* info = FontMgr.getFontInfo(i - kBuiltinReaderFontCount);
          if (info) {
            const bool loadable = ExternalFont::canFitGlyph(info->width, info->height);
            char label[80];
            snprintf(label, sizeof(label), "%s (%dpt)%s", info->name, info->size, loadable ? "" : " [!]");
            return std::string(label);
          }
        } else {
          if (i == 0) {
            return std::string(tr(STR_BUILTIN_DISABLED));
          }
          const FontInfo* info = FontMgr.getFontInfo(i - 1);
          if (info) {
            const bool loadable = ExternalFont::canFitGlyph(info->width, info->height);
            char label[80];
            snprintf(label, sizeof(label), "%s (%dpt)%s", info->name, info->size, loadable ? "" : " [!]");
            return std::string(label);
          }
        }
        return "";
      },
      nullptr, nullptr,
      [currentIndex](int i) -> std::string { return (i == currentIndex) ? std::string(tr(STR_ON_MARKER)) : std::string(""); });

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
