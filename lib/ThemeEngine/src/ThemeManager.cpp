#include "ThemeManager.h"

#include <SDCardManager.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <vector>

#include "DefaultTheme.h"
#include "LayoutElements.h"
#include "ListElement.h"

namespace ThemeEngine {

void ThemeManager::begin() {}

void ThemeManager::registerFont(const std::string& name, int id) { fontMap[name] = id; }

std::string ThemeManager::getAssetPath(const std::string& assetName) {
  // Check if absolute path
  if (!assetName.empty() && assetName[0] == '/') return assetName;

  // Otherwise relative to theme root
  std::string rootPath = "/themes/" + currentThemeName + "/" + assetName;
  if (SdMan.exists(rootPath.c_str())) return rootPath;

  // Fallback to assets/ subfolder
  return "/themes/" + currentThemeName + "/assets/" + assetName;
}

UIElement* ThemeManager::createElement(const std::string& id, const std::string& type) {
  // Basic elements
  if (type == "Container") return new Container(id);
  if (type == "Rectangle") return new Rectangle(id);
  if (type == "Label") return new Label(id);
  if (type == "Bitmap") return new BitmapElement(id);
  if (type == "List") return new List(id);
  if (type == "ProgressBar") return new ProgressBar(id);
  if (type == "Divider") return new Divider(id);

  // Layout elements
  if (type == "HStack") return new HStack(id);
  if (type == "VStack") return new VStack(id);
  if (type == "Grid") return new Grid(id);

  // Advanced elements
  if (type == "Badge") return new Badge(id);
  if (type == "Toggle") return new Toggle(id);
  if (type == "TabBar") return new TabBar(id);
  if (type == "Icon") return new Icon(id);
  if (type == "ScrollIndicator") return new ScrollIndicator(id);
  if (type == "BatteryIcon") return new BatteryIcon(id);

  return nullptr;
}

// Parse integer safely - returns 0 on error
static int parseIntSafe(const std::string& val) { return static_cast<int>(std::strtol(val.c_str(), nullptr, 10)); }

void ThemeManager::applyProperties(UIElement* elem, const std::map<std::string, std::string>& props) {
  const auto elemType = elem->getType();

  for (const auto& kv : props) {
    const std::string& key = kv.first;
    const std::string& val = kv.second;

    // Common properties
    if (key == "X")
      elem->setX(Dimension::parse(val));
    else if (key == "Y")
      elem->setY(Dimension::parse(val));
    else if (key == "Width")
      elem->setWidth(Dimension::parse(val));
    else if (key == "Height")
      elem->setHeight(Dimension::parse(val));
    else if (key == "Visible")
      elem->setVisibleExpr(val);
    else if (key == "Cacheable")
      elem->setCacheable(val == "true" || val == "1");

    // Rectangle properties
    else if (key == "Fill") {
      if (elemType == UIElement::ElementType::Rectangle) {
        auto rect = static_cast<Rectangle*>(elem);
        if (val.find('{') != std::string::npos) {
          rect->setFillExpr(val);
        } else {
          rect->setFill(val == "true" || val == "1");
        }
      }
    } else if (key == "Color") {
      if (elemType == UIElement::ElementType::Rectangle) {
        static_cast<Rectangle*>(elem)->setColorExpr(val);
      } else if (elemType == UIElement::ElementType::Container || elemType == UIElement::ElementType::HStack ||
                 elemType == UIElement::ElementType::VStack || elemType == UIElement::ElementType::Grid ||
                 elemType == UIElement::ElementType::TabBar) {
        static_cast<Container*>(elem)->setBackgroundColorExpr(val);
      } else if (elemType == UIElement::ElementType::Label) {
        static_cast<Label*>(elem)->setColorExpr(val);
      } else if (elemType == UIElement::ElementType::Divider) {
        static_cast<Divider*>(elem)->setColorExpr(val);
      } else if (elemType == UIElement::ElementType::Icon) {
        static_cast<Icon*>(elem)->setColorExpr(val);
      } else if (elemType == UIElement::ElementType::BatteryIcon) {
        static_cast<BatteryIcon*>(elem)->setColor(val);
      }
    }

    // Container properties
    else if (key == "Border") {
      if (auto c = elem->asContainer()) {
        if (val.find('{') != std::string::npos) {
          c->setBorderExpr(val);
        } else {
          c->setBorder(val == "true" || val == "1" || val == "yes");
        }
      }
    } else if (key == "Padding") {
      if (elemType == UIElement::ElementType::Container || elemType == UIElement::ElementType::HStack ||
          elemType == UIElement::ElementType::VStack || elemType == UIElement::ElementType::Grid) {
        static_cast<Container*>(elem)->setPadding(parseIntSafe(val));
      } else if (elemType == UIElement::ElementType::TabBar) {
        static_cast<TabBar*>(elem)->setPadding(parseIntSafe(val));
      }
    } else if (key == "BorderRadius") {
      if (elemType == UIElement::ElementType::Container || elemType == UIElement::ElementType::HStack ||
          elemType == UIElement::ElementType::VStack || elemType == UIElement::ElementType::Grid) {
        static_cast<Container*>(elem)->setBorderRadius(parseIntSafe(val));
      } else if (elemType == UIElement::ElementType::Bitmap) {
        static_cast<BitmapElement*>(elem)->setBorderRadius(parseIntSafe(val));
      } else if (elemType == UIElement::ElementType::Rectangle) {
        static_cast<Rectangle*>(elem)->setBorderRadius(parseIntSafe(val));
      } else if (elemType == UIElement::ElementType::Badge) {
        static_cast<Badge*>(elem)->setBorderRadius(parseIntSafe(val));
      } else if (elemType == UIElement::ElementType::Toggle) {
        static_cast<Toggle*>(elem)->setBorderRadius(parseIntSafe(val));
      }
    } else if (key == "Spacing") {
      if (elemType == UIElement::ElementType::HStack) {
        static_cast<HStack*>(elem)->setSpacing(parseIntSafe(val));
      } else if (elemType == UIElement::ElementType::VStack) {
        static_cast<VStack*>(elem)->setSpacing(parseIntSafe(val));
      } else if (elemType == UIElement::ElementType::List) {
        static_cast<List*>(elem)->setSpacing(parseIntSafe(val));
      }
    } else if (key == "CenterVertical") {
      if (elemType == UIElement::ElementType::HStack) {
        static_cast<HStack*>(elem)->setCenterVertical(val == "true" || val == "1");
      }
    } else if (key == "CenterHorizontal") {
      if (elemType == UIElement::ElementType::VStack) {
        static_cast<VStack*>(elem)->setCenterHorizontal(val == "true" || val == "1");
      }
    }

    // Label properties
    else if (key == "Text") {
      if (elemType == UIElement::ElementType::Label) {
        static_cast<Label*>(elem)->setText(val);
      } else if (elemType == UIElement::ElementType::Badge) {
        static_cast<Badge*>(elem)->setText(val);
      }
    } else if (key == "Font") {
      if (elemType == UIElement::ElementType::Label) {
        if (fontMap.count(val)) {
          static_cast<Label*>(elem)->setFont(fontMap[val]);
        }
      } else if (elemType == UIElement::ElementType::Badge) {
        if (fontMap.count(val)) {
          static_cast<Badge*>(elem)->setFont(fontMap[val]);
        }
      }
    } else if (key == "Centered") {
      if (elemType == UIElement::ElementType::Label) {
        static_cast<Label*>(elem)->setCentered(val == "true" || val == "1");
      }
    } else if (key == "Align") {
      if (elemType == UIElement::ElementType::Label) {
        Label::Alignment align = Label::Alignment::Left;
        if (val == "Center" || val == "center") align = Label::Alignment::Center;
        if (val == "Right" || val == "right") align = Label::Alignment::Right;
        static_cast<Label*>(elem)->setAlignment(align);
      }
    } else if (key == "MaxLines") {
      if (elemType == UIElement::ElementType::Label) {
        static_cast<Label*>(elem)->setMaxLines(parseIntSafe(val));
      }
    } else if (key == "Ellipsis") {
      if (elemType == UIElement::ElementType::Label) {
        static_cast<Label*>(elem)->setEllipsis(val == "true" || val == "1");
      }
    }

    // Bitmap/Icon properties
    else if (key == "Src") {
      if (elemType == UIElement::ElementType::Bitmap) {
        auto b = static_cast<BitmapElement*>(elem);
        if (val.find('{') == std::string::npos && val.find('/') == std::string::npos) {
          b->setSrc(getAssetPath(val));
        } else {
          b->setSrc(val);
        }
      } else if (elemType == UIElement::ElementType::Icon) {
        static_cast<Icon*>(elem)->setSrc(val);
      }
    } else if (key == "ScaleToFit") {
      if (elemType == UIElement::ElementType::Bitmap) {
        static_cast<BitmapElement*>(elem)->setScaleToFit(val == "true" || val == "1");
      }
    } else if (key == "PreserveAspect") {
      if (elemType == UIElement::ElementType::Bitmap) {
        static_cast<BitmapElement*>(elem)->setPreserveAspect(val == "true" || val == "1");
      }
    } else if (key == "IconSize") {
      if (elemType == UIElement::ElementType::Icon) {
        static_cast<Icon*>(elem)->setIconSize(parseIntSafe(val));
      }
    }

    // List properties
    else if (key == "Source") {
      if (elemType == UIElement::ElementType::List) {
        static_cast<List*>(elem)->setSource(val);
      }
    } else if (key == "ItemTemplate") {
      if (elemType == UIElement::ElementType::List) {
        static_cast<List*>(elem)->setItemTemplateId(val);
      }
    } else if (key == "ItemHeight") {
      if (elemType == UIElement::ElementType::List) {
        static_cast<List*>(elem)->setItemHeight(parseIntSafe(val));
      }
    } else if (key == "ItemWidth") {
      if (elemType == UIElement::ElementType::List) {
        static_cast<List*>(elem)->setItemWidth(parseIntSafe(val));
      }
    } else if (key == "Direction") {
      if (elemType == UIElement::ElementType::List) {
        static_cast<List*>(elem)->setDirectionFromString(val);
      }
    } else if (key == "Columns") {
      if (elemType == UIElement::ElementType::List) {
        static_cast<List*>(elem)->setColumns(parseIntSafe(val));
      } else if (elemType == UIElement::ElementType::Grid) {
        static_cast<Grid*>(elem)->setColumns(parseIntSafe(val));
      }
    } else if (key == "RowSpacing") {
      if (elemType == UIElement::ElementType::Grid) {
        static_cast<Grid*>(elem)->setRowSpacing(parseIntSafe(val));
      }
    } else if (key == "ColSpacing") {
      if (elemType == UIElement::ElementType::Grid) {
        static_cast<Grid*>(elem)->setColSpacing(parseIntSafe(val));
      }
    }

    // ProgressBar properties
    else if (key == "Value") {
      if (elemType == UIElement::ElementType::ProgressBar) {
        static_cast<ProgressBar*>(elem)->setValue(val);
      } else if (elemType == UIElement::ElementType::Toggle) {
        static_cast<Toggle*>(elem)->setValue(val);
      } else if (elemType == UIElement::ElementType::BatteryIcon) {
        static_cast<BatteryIcon*>(elem)->setValue(val);
      }
    } else if (key == "Max") {
      if (elemType == UIElement::ElementType::ProgressBar) {
        static_cast<ProgressBar*>(elem)->setMax(val);
      }
    } else if (key == "FgColor") {
      if (elemType == UIElement::ElementType::ProgressBar) {
        static_cast<ProgressBar*>(elem)->setFgColor(val);
      } else if (elemType == UIElement::ElementType::Badge) {
        static_cast<Badge*>(elem)->setFgColor(val);
      }
    } else if (key == "BgColor") {
      if (elemType == UIElement::ElementType::ProgressBar) {
        static_cast<ProgressBar*>(elem)->setBgColor(val);
      } else if (elemType == UIElement::ElementType::Badge) {
        static_cast<Badge*>(elem)->setBgColor(val);
      } else if (elemType == UIElement::ElementType::Container || elemType == UIElement::ElementType::HStack ||
                 elemType == UIElement::ElementType::VStack || elemType == UIElement::ElementType::Grid) {
        static_cast<Container*>(elem)->setBackgroundColorExpr(val);
      }
    } else if (key == "ShowBorder") {
      if (elemType == UIElement::ElementType::ProgressBar) {
        static_cast<ProgressBar*>(elem)->setShowBorder(val == "true" || val == "1");
      }
    }

    // Divider properties
    else if (key == "Horizontal") {
      if (elemType == UIElement::ElementType::Divider) {
        static_cast<Divider*>(elem)->setHorizontal(val == "true" || val == "1");
      }
    } else if (key == "Thickness") {
      if (elemType == UIElement::ElementType::Divider) {
        static_cast<Divider*>(elem)->setThickness(parseIntSafe(val));
      }
    }

    // Toggle properties
    else if (key == "OnColor") {
      if (elemType == UIElement::ElementType::Toggle) {
        static_cast<Toggle*>(elem)->setOnColor(val);
      }
    } else if (key == "OffColor") {
      if (elemType == UIElement::ElementType::Toggle) {
        static_cast<Toggle*>(elem)->setOffColor(val);
      }
    } else if (key == "KnobColor") {
      if (elemType == UIElement::ElementType::Toggle) {
        static_cast<Toggle*>(elem)->setKnobColor(val);
      }
    } else if (key == "TrackWidth") {
      if (elemType == UIElement::ElementType::Toggle) {
        static_cast<Toggle*>(elem)->setTrackWidth(parseIntSafe(val));
      } else if (elemType == UIElement::ElementType::ScrollIndicator) {
        static_cast<ScrollIndicator*>(elem)->setTrackWidth(parseIntSafe(val));
      }
    } else if (key == "TrackHeight") {
      if (elemType == UIElement::ElementType::Toggle) {
        static_cast<Toggle*>(elem)->setTrackHeight(parseIntSafe(val));
      }
    } else if (key == "KnobSize") {
      if (elemType == UIElement::ElementType::Toggle) {
        static_cast<Toggle*>(elem)->setKnobSize(parseIntSafe(val));
      }
    } else if (key == "KnobRadius") {
      if (elemType == UIElement::ElementType::Toggle) {
        static_cast<Toggle*>(elem)->setKnobRadius(parseIntSafe(val));
      }
    }

    // TabBar properties
    else if (key == "Selected") {
      if (elemType == UIElement::ElementType::TabBar) {
        static_cast<TabBar*>(elem)->setSelected(val);
      }
    } else if (key == "TabSpacing") {
      if (elemType == UIElement::ElementType::TabBar) {
        static_cast<TabBar*>(elem)->setTabSpacing(parseIntSafe(val));
      }
    } else if (key == "IndicatorHeight") {
      if (elemType == UIElement::ElementType::TabBar) {
        static_cast<TabBar*>(elem)->setIndicatorHeight(parseIntSafe(val));
      }
    } else if (key == "ShowIndicator") {
      if (elemType == UIElement::ElementType::TabBar) {
        static_cast<TabBar*>(elem)->setShowIndicator(val == "true" || val == "1");
      }
    }

    // ScrollIndicator properties
    else if (key == "Position") {
      if (elemType == UIElement::ElementType::ScrollIndicator) {
        static_cast<ScrollIndicator*>(elem)->setPosition(val);
      }
    } else if (key == "Total") {
      if (elemType == UIElement::ElementType::ScrollIndicator) {
        static_cast<ScrollIndicator*>(elem)->setTotal(val);
      }
    } else if (key == "VisibleCount") {
      if (elemType == UIElement::ElementType::ScrollIndicator) {
        static_cast<ScrollIndicator*>(elem)->setVisibleCount(val);
      }
    }

    // Badge properties
    else if (key == "PaddingH") {
      if (elemType == UIElement::ElementType::Badge) {
        static_cast<Badge*>(elem)->setPaddingH(parseIntSafe(val));
      }
    } else if (key == "PaddingV") {
      if (elemType == UIElement::ElementType::Badge) {
        static_cast<Badge*>(elem)->setPaddingV(parseIntSafe(val));
      }
    }
  }
}

const std::vector<uint8_t>* ThemeManager::getCachedAsset(const std::string& path) {
  if (assetCache.count(path)) {
    return &assetCache.at(path);
  }

  if (!SdMan.exists(path.c_str())) return nullptr;

  FsFile file;
  if (SdMan.openFileForRead("ThemeCache", path, file)) {
    size_t size = file.size();
    auto& buf = assetCache[path];
    buf.resize(size);
    file.read(buf.data(), size);
    file.close();
    return &buf;
  }
  return nullptr;
}

const ProcessedAsset* ThemeManager::getProcessedAsset(const std::string& path, GfxRenderer::Orientation orientation,
                                                      int targetW, int targetH) {
  std::string cacheKey = path;
  if (targetW > 0 && targetH > 0) {
    cacheKey += ":" + std::to_string(targetW) + "x" + std::to_string(targetH);
  }

  if (processedCache.count(cacheKey)) {
    const auto& asset = processedCache.at(cacheKey);
    if (asset.orientation == orientation) {
      return &asset;
    }
  }
  return nullptr;
}

void ThemeManager::cacheProcessedAsset(const std::string& path, const ProcessedAsset& asset, int targetW, int targetH) {
  std::string cacheKey = path;
  if (targetW > 0 && targetH > 0) {
    cacheKey += ":" + std::to_string(targetW) + "x" + std::to_string(targetH);
  }
  processedCache[cacheKey] = asset;
}

void ThemeManager::clearAssetCaches() {
  assetCache.clear();
  processedCache.clear();
}

void ThemeManager::unloadTheme() {
  for (auto& kv : elements) {
    delete kv.second;
  }
  elements.clear();
  clearAssetCaches();
  invalidateAllCaches();
}

void ThemeManager::invalidateAllCaches() {
  for (auto& kv : screenCaches) {
    kv.second.invalidate();
  }
}

void ThemeManager::invalidateScreenCache(const std::string& screenName) {
  if (screenCaches.count(screenName)) {
    screenCaches[screenName].invalidate();
  }
}

uint32_t ThemeManager::computeContextHash(const ThemeContext& context, const std::string& screenName) {
  uint32_t hash = 2166136261u;
  for (char c : screenName) {
    hash ^= static_cast<uint32_t>(c);
    hash *= 16777619u;
  }
  return hash;
}

void ThemeManager::loadTheme(const std::string& themeName) {
  unloadTheme();
  currentThemeName = themeName;

  std::map<std::string, std::map<std::string, std::string>> sections;

  if (themeName == "Default" || themeName.empty()) {
    std::string path = "/themes/Default/theme.ini";
    if (SdMan.exists(path.c_str())) {
      FsFile file;
      if (SdMan.openFileForRead("Theme", path, file)) {
        sections = IniParser::parse(file);
        file.close();
      }
    } else {
      sections = IniParser::parseString(getDefaultThemeIni());
    }
    currentThemeName = "Default";
  } else {
    std::string path = "/themes/" + themeName + "/theme.ini";

    if (!SdMan.exists(path.c_str())) {
      sections = IniParser::parseString(getDefaultThemeIni());
      currentThemeName = "Default";
    } else {
      FsFile file;
      if (SdMan.openFileForRead("Theme", path, file)) {
        sections = IniParser::parse(file);
        file.close();
      } else {
        sections = IniParser::parseString(getDefaultThemeIni());
        currentThemeName = "Default";
      }
    }
  }

  // Read theme configuration from [Global] section
  navBookCount = 1;
  if (sections.count("Global")) {
    const auto& global = sections.at("Global");
    if (global.count("NavBookCount")) {
      navBookCount = parseIntSafe(global.at("NavBookCount"));
      if (navBookCount < 1) navBookCount = 1;
      if (navBookCount > 10) navBookCount = 10;
    }
  }

  // Pass 1: Create elements
  for (const auto& sec : sections) {
    const std::string& id = sec.first;
    const std::map<std::string, std::string>& props = sec.second;

    if (id == "Global") continue;

    auto it = props.find("Type");
    if (it == props.end()) continue;

    const std::string& type = it->second;
    if (type.empty()) continue;

    UIElement* elem = createElement(id, type);
    if (elem) {
      elements[id] = elem;
    }
  }

  // Pass 2: Apply properties and wire parent relationships
  std::vector<List*> lists;
  for (const auto& sec : sections) {
    const std::string& id = sec.first;
    if (id == "Global") continue;
    if (elements.find(id) == elements.end()) continue;

    UIElement* elem = elements[id];
    applyProperties(elem, sec.second);

    if (elem->getType() == UIElement::ElementType::List) {
      lists.push_back(static_cast<List*>(elem));
    }

    // Wire parent relationship (fallback if Children not specified)
    if (sec.second.count("Parent")) {
      const std::string& parentId = sec.second.at("Parent");
      if (elements.count(parentId)) {
        UIElement* parent = elements[parentId];
        if (auto c = parent->asContainer()) {
          const auto& children = c->getChildren();
          if (std::find(children.begin(), children.end(), elem) == children.end()) {
            c->addChild(elem);
          }
        }
      }
    }

    // Children property - explicit ordering
    if (sec.second.count("Children")) {
      if (auto c = elem->asContainer()) {
        c->clearChildren();

        std::string s = sec.second.at("Children");
        size_t pos = 0;

        auto processChild = [&](const std::string& childName) {
          std::string childId = childName;
          size_t start = childId.find_first_not_of(" ");
          size_t end = childId.find_last_not_of(" ");
          if (start == std::string::npos) return;
          childId = childId.substr(start, end - start + 1);

          if (elements.count(childId)) {
            c->addChild(elements[childId]);
          }
        };

        while ((pos = s.find(',')) != std::string::npos) {
          processChild(s.substr(0, pos));
          s.erase(0, pos + 1);
        }
        processChild(s);
      }
    }
  }

  // Pass 3: Resolve list templates
  for (auto* l : lists) {
    l->resolveTemplate(elements);
  }
}

void ThemeManager::renderScreen(const std::string& screenName, const GfxRenderer& renderer,
                                const ThemeContext& context) {
  if (elements.count(screenName) == 0) {
    return;
  }

  UIElement* root = elements[screenName];
  root->layout(context, 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight());
  root->draw(renderer, context);
}

void ThemeManager::renderScreenOptimized(const std::string& screenName, const GfxRenderer& renderer,
                                         const ThemeContext& context, const ThemeContext* prevContext) {
  renderScreen(screenName, renderer, context);
}

}  // namespace ThemeEngine
