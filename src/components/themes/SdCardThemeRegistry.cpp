#include "SdCardThemeRegistry.h"

#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"
#include "ThemeInstaller.h"
#include "components/themes/lyra/LyraTheme.h"
#include "fontIds.h"

namespace {
constexpr int THEME_SCHEMA_VERSION = 1;
constexpr size_t MAX_PERSISTED_THEME_ID_LENGTH = sizeof(SETTINGS.sdThemeName) - 1;

void applyMetricOverrides(JsonObjectConst obj, ThemeMetrics& metrics) {
  if (obj.isNull()) return;
#define APPLY_INT_FIELD(name) metrics.name = obj[#name] | metrics.name
#define APPLY_BOOL_FIELD(name) metrics.name = obj[#name] | metrics.name
  APPLY_INT_FIELD(batteryWidth);
  APPLY_INT_FIELD(batteryHeight);
  APPLY_INT_FIELD(topPadding);
  APPLY_INT_FIELD(batteryBarHeight);
  APPLY_INT_FIELD(headerHeight);
  APPLY_INT_FIELD(verticalSpacing);
  APPLY_INT_FIELD(contentSidePadding);
  APPLY_INT_FIELD(listRowHeight);
  APPLY_INT_FIELD(listWithSubtitleRowHeight);
  APPLY_INT_FIELD(menuRowHeight);
  APPLY_INT_FIELD(menuSpacing);
  APPLY_INT_FIELD(tabSpacing);
  APPLY_INT_FIELD(tabBarHeight);
  APPLY_INT_FIELD(scrollBarWidth);
  APPLY_INT_FIELD(scrollBarRightOffset);
  APPLY_INT_FIELD(homeTopPadding);
  APPLY_INT_FIELD(homeCoverHeight);
  APPLY_INT_FIELD(homeCoverTileHeight);
  APPLY_INT_FIELD(homeRecentBooksCount);
  APPLY_BOOL_FIELD(homeContinueReadingInMenu);
  APPLY_BOOL_FIELD(homeShowContinueReadingHeader);
  APPLY_INT_FIELD(homeMenuTopOffset);
  APPLY_INT_FIELD(buttonHintsHeight);
  APPLY_INT_FIELD(sideButtonHintsWidth);
  APPLY_INT_FIELD(progressBarHeight);
  APPLY_INT_FIELD(progressBarMarginTop);
  APPLY_INT_FIELD(statusBarHorizontalMargin);
  APPLY_INT_FIELD(statusBarVerticalMargin);
  APPLY_INT_FIELD(keyboardKeyWidth);
  APPLY_INT_FIELD(keyboardKeyHeight);
  APPLY_INT_FIELD(keyboardKeySpacing);
  APPLY_INT_FIELD(keyboardBottomKeyHeight);
  APPLY_INT_FIELD(keyboardBottomKeySpacing);
  APPLY_BOOL_FIELD(keyboardBottomAligned);
  APPLY_BOOL_FIELD(keyboardCenteredText);
  APPLY_INT_FIELD(keyboardVerticalOffset);
  APPLY_INT_FIELD(keyboardTextFieldWidthPercent);
  APPLY_INT_FIELD(keyboardWidthPercent);
  APPLY_INT_FIELD(keyboardKeyCornerRadius);
  APPLY_BOOL_FIELD(keyboardFillUnselected);
  APPLY_BOOL_FIELD(keyboardOutlineAllUnselected);
  APPLY_BOOL_FIELD(keyboardDrawSpecialOutlineWhenUnselected);
  APPLY_INT_FIELD(keyboardSecondaryLabelRightPadding);
  APPLY_INT_FIELD(keyboardSecondaryLabelTopPadding);
  APPLY_INT_FIELD(keyboardMinArrowHeadSize);
  metrics.popupTopOffsetRatio = obj["popupTopOffsetRatio"] | metrics.popupTopOffsetRatio;
  APPLY_INT_FIELD(popupMarginX);
  APPLY_INT_FIELD(popupMarginY);
  APPLY_INT_FIELD(popupFrameThickness);
  APPLY_INT_FIELD(popupCornerRadius);
  APPLY_BOOL_FIELD(popupTextBold);
  APPLY_BOOL_FIELD(popupTextInverted);
  APPLY_INT_FIELD(popupTextBaselineOffsetY);
  APPLY_INT_FIELD(popupProgressBarHeight);
  APPLY_BOOL_FIELD(popupProgressDrawOutline);
  APPLY_BOOL_FIELD(popupProgressClampPercent);
  APPLY_BOOL_FIELD(popupProgressFillInverted);
  APPLY_BOOL_FIELD(popupProgressOutlineInverted);
  APPLY_INT_FIELD(textFieldHorizontalPadding);
  APPLY_INT_FIELD(textFieldNormalThickness);
  APPLY_INT_FIELD(textFieldCursorThickness);
  APPLY_INT_FIELD(textFieldLineEndOffset);
#undef APPLY_BOOL_FIELD
#undef APPLY_INT_FIELD
}

ThemeSlotX parseSlotX(const char* value) {
  if (value == nullptr) return ThemeSlotX::Center;
  if (strcmp(value, "padding") == 0) return ThemeSlotX::Padding;
  if (strcmp(value, "right-padding") == 0) return ThemeSlotX::RightPadding;
  return ThemeSlotX::Center;
}

ThemeSlotY parseSlotY(const char* value) {
  if (value == nullptr) return ThemeSlotY::Top;
  if (strcmp(value, "center") == 0 || strcmp(value, "centerY") == 0) return ThemeSlotY::Center;
  return ThemeSlotY::Top;
}

ThemeBookRef parseBookRef(const char* value) {
  if (value == nullptr) return ThemeBookRef::Selected;
  if (strcmp(value, "previous") == 0) return ThemeBookRef::Previous;
  if (strcmp(value, "next") == 0) return ThemeBookRef::Next;
  if (strcmp(value, "index") == 0) return ThemeBookRef::Index;
  return ThemeBookRef::Selected;
}

void parseTitleSpec(JsonObjectConst obj, ThemeTitleSpec& title) {
  if (obj.isNull()) return;
  title.enabled = obj["enabled"] | true;
  title.fontId = obj["fontId"] | title.fontId;
  const char* font = obj["font"].as<const char*>();
  if (font != nullptr) {
    if (strcmp(font, "ui10") == 0) {
      title.fontId = UI_10_FONT_ID;
    } else if (strcmp(font, "small") == 0) {
      title.fontId = SMALL_FONT_ID;
    } else {
      title.fontId = UI_12_FONT_ID;
    }
  } else if (title.fontId == 10) {
    title.fontId = UI_10_FONT_ID;
  } else if (title.fontId == 12) {
    title.fontId = UI_12_FONT_ID;
  }
  title.bold = obj["bold"] | title.bold;
  title.maxLines = obj["maxLines"] | title.maxLines;
  title.offsetY = obj["offsetY"] | title.offsetY;

  const char* style = obj["style"].as<const char*>();
  if (style != nullptr) {
    title.bold = strcmp(style, "bold") == 0;
  }
}

void parseCoverSlot(JsonObjectConst obj, ThemeCoverSlotSpec& slot) {
  if (obj.isNull()) return;
  slot.book = parseBookRef(obj["book"].as<const char*>());
  slot.bookIndex = obj["bookIndex"] | slot.bookIndex;
  slot.x = parseSlotX(obj["x"].as<const char*>());
  slot.y = parseSlotY(obj["y"].as<const char*>());
  slot.height = obj["height"] | slot.height;
  slot.widthPercent = obj["widthPercent"] | slot.widthPercent;
  slot.xOffset = obj["xOffset"] | slot.xOffset;
  slot.yOffset = obj["yOffset"] | slot.yOffset;
  slot.selected = obj["selected"] | slot.selected;
  parseTitleSpec(obj["title"].as<JsonObjectConst>(), slot.title);
}

void parseHomeRecentsSpec(JsonObjectConst obj, ThemeHomeRecentsSpec& spec) {
  if (obj.isNull()) return;
  const char* type = obj["type"].as<const char*>();
  if (type != nullptr) {
    if (strcmp(type, "cover-strip") == 0) {
      spec.type = ThemeHomeRecentsType::CoverStrip;
    } else if (strcmp(type, "none") == 0) {
      spec.type = ThemeHomeRecentsType::None;
    }
  }
  spec.maxBooks = obj["maxBooks"] | spec.maxBooks;
  spec.wrap = obj["wrap"] | spec.wrap;
  spec.drawPanel = obj["drawPanel"] | spec.drawPanel;
  spec.panelCornerRadius = obj["panelCornerRadius"] | spec.panelCornerRadius;
  spec.panelInsetX = obj["panelInsetX"] | spec.panelInsetX;
  spec.selectionLineWidth = obj["selectionLineWidth"] | spec.selectionLineWidth;
  spec.inactiveSelectionLineWidth = obj["inactiveSelectionLineWidth"] | spec.inactiveSelectionLineWidth;
  spec.selectionCornerRadius = obj["selectionCornerRadius"] | spec.selectionCornerRadius;

  JsonArrayConst slots = obj["slots"].as<JsonArrayConst>();
  if (!slots.isNull()) {
    if (spec.type == ThemeHomeRecentsType::Default) {
      spec.type = ThemeHomeRecentsType::CoverStrip;
    }
    spec.slots.clear();
    for (JsonObjectConst slotObj : slots) {
      if (spec.slots.size() >= 5) break;
      ThemeCoverSlotSpec slot;
      parseCoverSlot(slotObj, slot);
      spec.slots.push_back(slot);
    }
  }
}

void applyFontSpec(JsonObjectConst obj, int& fontId, bool& bold) {
  const char* font = obj["font"].as<const char*>();
  if (font != nullptr) {
    if (strcmp(font, "ui10") == 0) {
      fontId = UI_10_FONT_ID;
    } else if (strcmp(font, "small") == 0) {
      fontId = SMALL_FONT_ID;
    } else {
      fontId = UI_12_FONT_ID;
    }
  } else {
    fontId = obj["fontId"] | fontId;
    if (fontId == 10) {
      fontId = UI_10_FONT_ID;
    } else if (fontId == 12) {
      fontId = UI_12_FONT_ID;
    }
  }

  bold = obj["bold"] | bold;
  const char* style = obj["style"].as<const char*>();
  if (style != nullptr) {
    bold = strcmp(style, "bold") == 0;
  }
}

void parseButtonMenuSpec(JsonObjectConst obj, ThemeButtonMenuSpec& spec) {
  if (obj.isNull()) return;
  spec.enabled = true;
  applyFontSpec(obj, spec.fontId, spec.bold);
  spec.centeredText = obj["centeredText"] | spec.centeredText;
  spec.centerVertically = obj["centerVertically"] | spec.centerVertically;
  spec.showIcons = obj["showIcons"] | spec.showIcons;
  spec.panelWidth = obj["panelWidth"] | spec.panelWidth;
  spec.drawPanel = obj["drawPanel"] | spec.drawPanel;
  spec.panelCornerRadius = obj["panelCornerRadius"] | spec.panelCornerRadius;
  spec.selectionCornerRadius = obj["selectionCornerRadius"] | spec.selectionCornerRadius;
  spec.selectionInset = obj["selectionInset"] | spec.selectionInset;
  spec.selectedTextInverted = obj["selectedTextInverted"] | spec.selectedTextInverted;
  spec.selectionFillBlack = obj["selectionFillBlack"] | spec.selectionFillBlack;

  const char* selectionStyle = obj["selectionStyle"].as<const char*>();
  if (selectionStyle != nullptr) {
    if (strcmp(selectionStyle, "outline") == 0) {
      spec.selectionStyle = ThemeMenuSelectionStyle::Outline;
    } else if (strcmp(selectionStyle, "triangle") == 0) {
      spec.selectionStyle = ThemeMenuSelectionStyle::Triangle;
    } else if (strcmp(selectionStyle, "underline") == 0) {
      spec.selectionStyle = ThemeMenuSelectionStyle::Underline;
    } else if (strcmp(selectionStyle, "pill") == 0) {
      spec.selectionStyle = ThemeMenuSelectionStyle::Pill;
    } else {
      spec.selectionStyle = ThemeMenuSelectionStyle::Fill;
    }
  }
  spec.rowPaddingX = obj["rowPaddingX"] | spec.rowPaddingX;
  spec.textInsetX = obj["textInsetX"] | spec.textInsetX;
}

void parseListSpec(JsonObjectConst obj, ThemeListSpec& spec) {
  if (obj.isNull()) return;
  spec.enabled = true;
  applyFontSpec(obj, spec.fontId, spec.bold);
  spec.subtitleFontId = obj["subtitleFontId"] | spec.subtitleFontId;
  spec.valueFontId = obj["valueFontId"] | spec.valueFontId;
  spec.showIcons = obj["showIcons"] | spec.showIcons;
  spec.iconSize = obj["iconSize"] | spec.iconSize;
  spec.textGap = obj["textGap"] | spec.textGap;
  const char* selectionStyle = obj["selectionStyle"].as<const char*>();
  if (selectionStyle != nullptr) {
    if (strcmp(selectionStyle, "underline") == 0) {
      spec.selectionStyle = ThemeMenuSelectionStyle::Underline;
    } else if (strcmp(selectionStyle, "outline") == 0) {
      spec.selectionStyle = ThemeMenuSelectionStyle::Outline;
    } else {
      spec.selectionStyle = ThemeMenuSelectionStyle::Fill;
    }
  }
  spec.selectionCornerRadius = obj["selectionCornerRadius"] | spec.selectionCornerRadius;
  spec.selectionFill = obj["selectionFill"] | spec.selectionFill;
  spec.selectionOutline = obj["selectionOutline"] | spec.selectionOutline;
  spec.selectedTextInverted = obj["selectedTextInverted"] | spec.selectedTextInverted;
  spec.rowBackgrounds = obj["rowBackgrounds"] | spec.rowBackgrounds;
  spec.centerSingleLineRows = obj["centerSingleLineRows"] | spec.centerSingleLineRows;
  spec.subtitleRowAutoHeight = obj["subtitleRowAutoHeight"] | spec.subtitleRowAutoHeight;
  spec.centerValueVertically = obj["centerValueVertically"] | spec.centerValueVertically;
  spec.rowSidePadding = obj["rowSidePadding"] | spec.rowSidePadding;
  spec.rowGap = obj["rowGap"] | spec.rowGap;
  spec.textInsetX = obj["textInsetX"] | spec.textInsetX;
  spec.selectionInsetX = obj["selectionInsetX"] | spec.selectionInsetX;
  spec.selectionInsetY = obj["selectionInsetY"] | spec.selectionInsetY;
  spec.titleOffsetY = obj["titleOffsetY"] | spec.titleOffsetY;
  spec.subtitleOffsetY = obj["subtitleOffsetY"] | spec.subtitleOffsetY;
  spec.subtitleTopPadding = obj["subtitleTopPadding"] | spec.subtitleTopPadding;
  spec.subtitleBottomPadding = obj["subtitleBottomPadding"] | spec.subtitleBottomPadding;
  spec.subtitleInterLineGap = obj["subtitleInterLineGap"] | spec.subtitleInterLineGap;
  spec.valueOffsetY = obj["valueOffsetY"] | spec.valueOffsetY;
  spec.subtitleValueOffsetY = obj["subtitleValueOffsetY"] | spec.subtitleValueOffsetY;
  spec.iconOffsetY = obj["iconOffsetY"] | spec.iconOffsetY;

  if (spec.subtitleFontId == 0) spec.subtitleFontId = SMALL_FONT_ID;
  if (spec.valueFontId == 0) spec.valueFontId = spec.fontId;
}

void parseButtonHintsSpec(JsonObjectConst obj, ThemeButtonHintsSpec& spec) {
  if (obj.isNull()) return;
  spec.enabled = true;
  applyFontSpec(obj, spec.fontId, spec.bold);
  spec.buttonWidth = obj["buttonWidth"] | spec.buttonWidth;
  spec.smallButtonHeight = obj["smallButtonHeight"] | spec.smallButtonHeight;
  spec.cornerRadius = obj["cornerRadius"] | spec.cornerRadius;
  spec.fill = obj["fill"] | spec.fill;
  spec.outline = obj["outline"] | spec.outline;
  spec.drawEmpty = obj["drawEmpty"] | spec.drawEmpty;
  spec.shapes = obj["shapes"] | spec.shapes;
  const char* hintLayout = obj["layout"].as<const char*>();
  if (hintLayout != nullptr) {
    if (strcmp(hintLayout, "shapes") == 0) {
      spec.style = ThemeButtonHintsStyle::Shapes;
      spec.shapes = true;
    } else if (strcmp(hintLayout, "groups") == 0) {
      spec.style = ThemeButtonHintsStyle::Groups;
      spec.shapes = false;
    } else {
      spec.style = ThemeButtonHintsStyle::Buttons;
    }
  } else if (spec.shapes) {
    spec.style = ThemeButtonHintsStyle::Shapes;
  }
  spec.sidePadding = obj["sidePadding"] | spec.sidePadding;
  spec.groupGap = obj["groupGap"] | spec.groupGap;
  spec.bottomMargin = obj["bottomMargin"] | spec.bottomMargin;
  spec.innerPadding = obj["innerPadding"] | spec.innerPadding;
  spec.shapeSize = obj["shapeSize"] | spec.shapeSize;
  spec.textOffsetY = obj["textOffsetY"] | spec.textOffsetY;
  if (spec.fontId == 0) spec.fontId = SMALL_FONT_ID;
}

void parseTabBarSpec(JsonObjectConst obj, ThemeTabBarSpec& spec) {
  if (obj.isNull()) return;
  spec.enabled = true;
  applyFontSpec(obj, spec.fontId, spec.bold);
  spec.equalWidth = obj["equalWidth"] | spec.equalWidth;
  const char* selectionStyle = obj["selectionStyle"].as<const char*>();
  if (selectionStyle != nullptr) {
    if (strcmp(selectionStyle, "underline") == 0) {
      spec.selectionStyle = ThemeMenuSelectionStyle::Underline;
    } else {
      spec.selectionStyle = ThemeMenuSelectionStyle::Fill;
    }
  }
  spec.selectedCornerRadius = obj["selectedCornerRadius"] | spec.selectedCornerRadius;
  spec.selectedTextInverted = obj["selectedTextInverted"] | spec.selectedTextInverted;
  spec.drawDivider = obj["drawDivider"] | spec.drawDivider;
  spec.horizontalInset = obj["horizontalInset"] | spec.horizontalInset;
}

void parseHeaderSpec(JsonObjectConst obj, ThemeHeaderSpec& spec) {
  if (obj.isNull()) return;
  spec.enabled = true;
  applyFontSpec(obj, spec.fontId, spec.bold);
  spec.centeredTitle = obj["centeredTitle"] | spec.centeredTitle;
  spec.showDivider = obj["showDivider"] | spec.showDivider;
  spec.titleOffsetY = obj["titleOffsetY"] | spec.titleOffsetY;
  spec.batteryOffsetY = obj["batteryOffsetY"] | spec.batteryOffsetY;
}

bool iconForKey(const char* key, UIIcon& out) {
  if (key == nullptr) return false;
  if (strcmp(key, "folder") == 0 || strcmp(key, "folder24") == 0)
    out = UIIcon::Folder;
  else if (strcmp(key, "text") == 0 || strcmp(key, "text24") == 0)
    out = UIIcon::Text;
  else if (strcmp(key, "image") == 0 || strcmp(key, "image24") == 0)
    out = UIIcon::Image;
  else if (strcmp(key, "book") == 0 || strcmp(key, "book24") == 0)
    out = UIIcon::Book;
  else if (strcmp(key, "file") == 0 || strcmp(key, "file24") == 0)
    out = UIIcon::File;
  else if (strcmp(key, "recent") == 0)
    out = UIIcon::Recent;
  else if (strcmp(key, "settings") == 0 || strcmp(key, "settings2") == 0)
    out = UIIcon::Settings;
  else if (strcmp(key, "transfer") == 0)
    out = UIIcon::Transfer;
  else if (strcmp(key, "library") == 0)
    out = UIIcon::Library;
  else if (strcmp(key, "wifi") == 0)
    out = UIIcon::Wifi;
  else if (strcmp(key, "hotspot") == 0)
    out = UIIcon::Hotspot;
  else if (strcmp(key, "bookmark") == 0)
    out = UIIcon::Bookmark;
  else
    return false;
  return true;
}

void parseIconMap(JsonObjectConst obj, ThemeIconMap& icons) {
  if (obj.isNull()) return;
  for (JsonPairConst kv : obj) {
    UIIcon icon = UIIcon::None;
    const char* path = kv.value().as<const char*>();
    if (iconForKey(kv.key().c_str(), icon) && path != nullptr && ThemeInstaller::isValidRelativePath(path)) {
      if (strstr(kv.key().c_str(), "24") != nullptr && icons.find(icon) != icons.end()) continue;
      icons[icon] = path;
    }
  }
}

ThemeMetrics defaultMetrics() { return LyraMetrics::values; }
}  // namespace

const char* SdCardThemeRegistry::activeDeviceId() { return gpio.deviceIsX3() ? "x3" : "x4"; }

bool SdCardThemeRegistry::isSafeId(const char* value) {
  if (value == nullptr || value[0] == '\0') return false;
  if (strstr(value, "..") != nullptr || strchr(value, '/') != nullptr || strchr(value, '\\') != nullptr) return false;
  for (const char* p = value; *p != '\0'; ++p) {
    const char c = *p;
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != ' ') return false;
  }
  return true;
}

bool SdCardThemeRegistry::isSafeThemeId(const char* value) {
  if (value == nullptr || value[0] == '\0') return false;
  if (strlen(value) > MAX_PERSISTED_THEME_ID_LENGTH) return false;
  if (strstr(value, "..") != nullptr || strchr(value, '/') != nullptr || strchr(value, '\\') != nullptr) return false;
  for (const char* p = value; *p != '\0'; ++p) {
    const char c = *p;
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
  }
  return true;
}

bool SdCardThemeRegistry::parseThemeJson(const char* themeDirPath, SdCardThemeInfo& out) {
  char jsonPath[180];
  snprintf(jsonPath, sizeof(jsonPath), "%s/theme.json", themeDirPath);

  HalFile file;
  if (!Storage.openFileForRead("THREG", jsonPath, file)) {
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    LOG_ERR("THREG", "Theme JSON parse error in %s: %s", jsonPath, err.c_str());
    return false;
  }

  const int schema = doc["schema"] | 0;
  if (schema != THEME_SCHEMA_VERSION) {
    LOG_ERR("THREG", "Unsupported theme schema %d in %s", schema, jsonPath);
    return false;
  }

  const char* id = doc["id"] | "";
  const char* name = doc["name"] | id;
  if (!isSafeThemeId(id) || !isSafeId(name)) {
    LOG_ERR("THREG", "Invalid theme id/name in %s", jsonPath);
    return false;
  }

  const char* deviceId = activeDeviceId();
  JsonObject deviceObj = doc["devices"][deviceId].as<JsonObject>();

  const char* inherits = deviceObj["inherits"] | doc["inherits"] | "lyra";
  out.id = id;
  out.name = name;
  out.path = themeDirPath;
  out.inherits = inherits;
  out.deviceId = deviceId;
  out.metrics = defaultMetrics();
  parseHomeRecentsSpec(doc["components"]["homeRecents"].as<JsonObjectConst>(), out.homeRecents);
  parseHomeRecentsSpec(deviceObj["components"]["homeRecents"].as<JsonObjectConst>(), out.homeRecents);
  parseButtonMenuSpec(doc["components"]["homeMenu"].as<JsonObjectConst>(), out.buttonMenu);
  parseButtonMenuSpec(deviceObj["components"]["homeMenu"].as<JsonObjectConst>(), out.buttonMenu);
  parseListSpec(doc["components"]["list"].as<JsonObjectConst>(), out.list);
  parseListSpec(deviceObj["components"]["list"].as<JsonObjectConst>(), out.list);
  parseButtonHintsSpec(doc["components"]["buttonHints"].as<JsonObjectConst>(), out.buttonHints);
  parseButtonHintsSpec(deviceObj["components"]["buttonHints"].as<JsonObjectConst>(), out.buttonHints);
  parseTabBarSpec(doc["components"]["tabBar"].as<JsonObjectConst>(), out.tabBar);
  parseTabBarSpec(deviceObj["components"]["tabBar"].as<JsonObjectConst>(), out.tabBar);
  parseHeaderSpec(doc["components"]["header"].as<JsonObjectConst>(), out.header);
  parseHeaderSpec(deviceObj["components"]["header"].as<JsonObjectConst>(), out.header);
  applyMetricOverrides(doc["metrics"].as<JsonObjectConst>(), out.metrics);
  applyMetricOverrides(deviceObj["metrics"].as<JsonObjectConst>(), out.metrics);
  if ((out.buttonMenu.enabled && out.buttonMenu.showIcons) || (out.list.enabled && out.list.showIcons)) {
    parseIconMap(doc["assets"]["icons"].as<JsonObjectConst>(), out.icons);
    parseIconMap(deviceObj["assets"]["icons"].as<JsonObjectConst>(), out.icons);
  }
  if (out.homeRecents.type == ThemeHomeRecentsType::CoverStrip) {
    out.metrics.homeRecentBooksCount = std::max(1, out.homeRecents.maxBooks);
  } else if (out.homeRecents.type == ThemeHomeRecentsType::None) {
    out.metrics.homeCoverHeight = 0;
    out.metrics.homeCoverTileHeight = 0;
  }
  out.constraints.screenWidth = deviceObj["constraints"]["screenWidth"] | doc["constraints"]["screenWidth"] | 0;
  out.constraints.screenHeight = deviceObj["constraints"]["screenHeight"] | doc["constraints"]["screenHeight"] | 0;
  out.constraints.frontButtons = deviceObj["constraints"]["frontButtons"] | doc["constraints"]["frontButtons"] | 0;
  out.constraints.sideButtons = (deviceObj["constraints"]["sideButtons"] | doc["constraints"]["sideButtons"] | "");
  return true;
}

void SdCardThemeRegistry::scanRoot(const char* rootPath, std::vector<SdCardThemeInfo>& out) {
  HalFile root = Storage.open(rootPath);
  if (!root) {
    LOG_DBG("THREG", "Themes directory not found: %s", rootPath);
    return;
  }
  if (!root.isDirectory()) {
    LOG_ERR("THREG", "Themes path is not a directory: %s", rootPath);
    return;
  }

  char nameBuffer[128];
  while (true) {
    HalFile entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;
    if (!isSafeThemeId(nameBuffer)) continue;

    char themeDirPath[180];
    snprintf(themeDirPath, sizeof(themeDirPath), "%s/%s", rootPath, nameBuffer);

    SdCardThemeInfo info;
    if (!parseThemeJson(themeDirPath, info)) continue;

    bool exists = false;
    for (const auto& theme : out) {
      if (theme.id == info.id) {
        exists = true;
        break;
      }
    }
    if (exists) continue;

    LOG_DBG("THREG", "Found theme: %s (%s)", info.name.c_str(), info.path.c_str());
    out.push_back(std::move(info));
  }
}

bool SdCardThemeRegistry::discover() {
  themes_.clear();
  themes_.reserve(8);

  scanRoot(THEMES_DIR_HIDDEN, themes_);
  scanRoot(THEMES_DIR_VISIBLE, themes_);

  std::sort(themes_.begin(), themes_.end(),
            [](const SdCardThemeInfo& a, const SdCardThemeInfo& b) { return a.name < b.name; });

  if (static_cast<int>(themes_.size()) > MAX_SD_THEMES) {
    themes_.resize(MAX_SD_THEMES);
  }

  LOG_DBG("THREG", "Discovery complete: %d themes", static_cast<int>(themes_.size()));
  return !themes_.empty();
}

void SdCardThemeRegistry::clear() {
  themes_.clear();
  themes_.shrink_to_fit();
}

const SdCardThemeInfo* SdCardThemeRegistry::findTheme(const std::string& id) const {
  auto it = std::find_if(themes_.begin(), themes_.end(),
                         [&](const SdCardThemeInfo& theme) { return theme.id == id || theme.name == id; });
  return it == themes_.end() ? nullptr : &*it;
}

const char* SdCardThemeRegistry::findThemeRoot(const char* themeId) {
  if (!isSafeThemeId(themeId)) return nullptr;
  char path[180];
  snprintf(path, sizeof(path), "%s/%s", THEMES_DIR_HIDDEN, themeId);
  if (Storage.exists(path)) return THEMES_DIR_HIDDEN;
  snprintf(path, sizeof(path), "%s/%s", THEMES_DIR_VISIBLE, themeId);
  if (Storage.exists(path)) return THEMES_DIR_VISIBLE;
  return nullptr;
}

const char* SdCardThemeRegistry::defaultWriteRoot() {
  const bool hiddenExists = Storage.exists(THEMES_DIR_HIDDEN);
  const bool visibleExists = Storage.exists(THEMES_DIR_VISIBLE);
  if (hiddenExists) return THEMES_DIR_HIDDEN;
  if (visibleExists) return THEMES_DIR_VISIBLE;
  return THEMES_DIR_HIDDEN;
}
