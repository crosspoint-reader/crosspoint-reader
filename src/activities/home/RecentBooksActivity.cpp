#include "RecentBooksActivity.h"

#include <ArduinoJson.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "core/features/FeatureModules.h"
#include "fontIds.h"
#include "util/PokemonBookDataStore.h"
#include "util/StringUtils.h"

namespace {
constexpr int kPartyMaxBooks = 6;

std::string fallbackTitleFromPath(const std::string& path) {
  auto title = path;
  const size_t lastSlash = title.find_last_of('/');
  if (lastSlash != std::string::npos) {
    title = title.substr(lastSlash + 1);
  }

  if (StringUtils::checkFileExtension(title, ".xtch")) {
    title.resize(title.length() - 5);
  } else if (StringUtils::checkFileExtension(title, ".epub") || StringUtils::checkFileExtension(title, ".xtc") ||
             StringUtils::checkFileExtension(title, ".txt") || StringUtils::checkFileExtension(title, ".md")) {
    title.resize(title.length() - 4);
  }

  return title;
}

std::string titleCase(const std::string& value) {
  std::string result = value;
  bool capitalize = true;
  for (char& c : result) {
    if (c == '-' || c == '_' || c == ' ') {
      if (c == '_' || c == '-') {
        c = ' ';
      }
      capitalize = true;
      continue;
    }
    const unsigned char uc = static_cast<unsigned char>(c);
    c = capitalize ? static_cast<char>(std::toupper(uc)) : static_cast<char>(std::tolower(uc));
    capitalize = false;
  }
  return result;
}

std::string summarizePokemonLabel(JsonObjectConst pokemon) {
  const int pokemonId = pokemon["id"] | 0;
  const char* rawName = pokemon["name"] | "";
  if (rawName[0] == '\0') {
    rawName = pokemon["speciesName"] | "";
  }
  const std::string name = titleCase(rawName);
  if (pokemonId <= 0) {
    return name.empty() ? "Pokemon assigned" : name;
  }
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "#%04d %s", pokemonId, name.c_str());
  return buffer;
}

std::string summarizeCurrentForm(JsonArrayConst chain, const int level, const std::string& fallbackName) {
  std::string current = fallbackName;
  for (JsonVariantConst item : chain) {
    if (!item.is<JsonObjectConst>()) {
      continue;
    }
    const JsonObjectConst stage = item.as<JsonObjectConst>();
    const std::string stageName = titleCase(stage["name"] | "");
    const bool hasMinLevel = stage["minLevel"].is<int>();
    const int minLevel = hasMinLevel ? stage["minLevel"].as<int>() : -1;

    if (current.empty() && !stageName.empty()) {
      current = stageName;
    }
    if (hasMinLevel && level >= minLevel && !stageName.empty()) {
      current = stageName;
    }
  }
  return current;
}

std::string summarizeNextForm(JsonArrayConst chain, const int level) {
  for (JsonVariantConst item : chain) {
    if (!item.is<JsonObjectConst>()) {
      continue;
    }
    const JsonObjectConst stage = item.as<JsonObjectConst>();
    if (!stage["minLevel"].is<int>()) {
      continue;
    }
    const int minLevel = stage["minLevel"].as<int>();
    if (level < minLevel) {
      const std::string nextName = titleCase(stage["name"] | "");
      if (nextName.empty()) {
        return "";
      }
      return "Next: " + nextName + " @Lv " + std::to_string(minLevel);
    }
  }
  return level >= 100 ? "Book complete at Lv 100" : "Final form reached";
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  partyMode = core::FeatureModules::hasCapability(core::Capability::PokemonParty);
  const auto& books = RECENT_BOOKS.getBooks();
  const size_t limit = partyMode ? std::min(static_cast<size_t>(kPartyMaxBooks), books.size()) : books.size();
  recentBooks.reserve(limit);

  for (const auto& book : books) {
    if (recentBooks.size() >= limit) {
      break;
    }

    // Skip if file no longer exists
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }

    PartyBookEntry entry;
    entry.book = book;
    if (entry.book.title.empty()) {
      entry.book.title = fallbackTitleFromPath(entry.book.path);
    }

    if (partyMode) {
      entry.hasProgress = BookProgressDataStore::loadProgress(entry.book.path, entry.progress);
      if (entry.hasProgress) {
        entry.level = std::max(1, static_cast<int>(std::floor(entry.progress.percent)));
        entry.progressLabel =
            "Lv " + std::to_string(entry.level) + "  " + BookProgressDataStore::formatPositionLabel(entry.progress);
      } else {
        entry.progressLabel = "Lv 1  Not started";
      }

      JsonDocument pokemonDoc;
      if (PokemonBookDataStore::loadPokemonDocument(entry.book.path, pokemonDoc) &&
          pokemonDoc["pokemon"].is<JsonObjectConst>()) {
        const JsonObjectConst pokemon = pokemonDoc["pokemon"].as<JsonObjectConst>();
        entry.hasPokemon = true;
        entry.pokemonLabel = summarizePokemonLabel(pokemon);

        const JsonArrayConst chain = pokemon["evolutionChain"].as<JsonArrayConst>();
        const char* fallbackRawName = pokemon["speciesName"] | "";
        if (fallbackRawName[0] == '\0') {
          fallbackRawName = pokemon["name"] | "";
        }
        const std::string fallbackName = titleCase(fallbackRawName);
        entry.currentFormLabel = "Form: " + summarizeCurrentForm(chain, entry.level, fallbackName);
        entry.nextFormLabel = summarizeNextForm(chain, entry.level);
      } else {
        entry.pokemonLabel = "No Pokemon assigned";
        entry.currentFormLabel = "Assign from the web Pokedex";
        entry.nextFormLabel.clear();
      }
    }

    recentBooks.push_back(std::move(entry));
  }
}

bool RecentBooksActivity::drawCoverAt(const std::string& coverPath, const int x, const int y, const int width,
                                      const int height) const {
  if (coverPath.empty() || !Storage.exists(coverPath.c_str())) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("PTY", coverPath, file)) {
    return false;
  }

  Bitmap bitmap(file);
  const bool ok = bitmap.parseHeaders() == BmpReaderError::Ok;
  if (ok) {
    renderer.drawBitmap(bitmap, x, y, width, height);
  }
  file.close();
  return ok;
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < recentBooks.size()) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].book.path.c_str());
      onSelectBook(recentBooks[selectorIndex].book.path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  int listSize = static_cast<int>(recentBooks.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 partyMode ? "Party" : "Recent Books", partyMode ? "6 recent books with Pokemon progress" : nullptr);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20,
                      partyMode ? "Open books to build your party" : "No recent books");
  } else if (partyMode) {
    const int slotCount = static_cast<int>(recentBooks.size());
    const int slotGap = 8;
    const int slotHeight = (contentHeight - slotGap * std::max(0, slotCount - 1)) / std::max(1, slotCount);
    const int slotWidth = pageWidth - metrics.contentSidePadding * 2;
    const int coverHeight = std::max(56, slotHeight - 18);
    const int coverWidth = coverHeight * 3 / 4;

    for (int i = 0; i < slotCount; ++i) {
      const auto& entry = recentBooks[static_cast<size_t>(i)];
      const bool selected = static_cast<int>(selectorIndex) == i;
      const int y = contentTop + i * (slotHeight + slotGap);
      const int x = metrics.contentSidePadding;
      const int textX = x + 12 + coverWidth + 12;
      const int textW = slotWidth - (textX - x) - 12;
      const int titleY = y + 8;
      const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

      if (selected) {
        renderer.fillRect(x, y, slotWidth, slotHeight);
      } else {
        renderer.drawRect(x, y, slotWidth, slotHeight);
      }

      const std::string thumbPath = UITheme::getCoverThumbPath(entry.book.coverBmpPath, coverHeight);
      if (!drawCoverAt(thumbPath, x + 8, y + 8, coverWidth, coverHeight)) {
        renderer.drawRect(x + 8, y + 8, coverWidth, coverHeight, !selected);
        const int placeholderY = y + 8 + (coverHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
        const int placeholderX = x + 8 + std::max(0, (coverWidth - renderer.getTextWidth(UI_10_FONT_ID, "BOOK")) / 2);
        renderer.drawText(UI_10_FONT_ID, placeholderX, placeholderY, "BOOK", !selected);
      }

      renderer.drawText(UI_12_FONT_ID, textX, titleY,
                        renderer.truncatedText(UI_12_FONT_ID, entry.book.title.c_str(), textW).c_str(), !selected);
      renderer.drawText(UI_10_FONT_ID, textX, titleY + lineH + 2,
                        renderer.truncatedText(UI_10_FONT_ID, entry.pokemonLabel.c_str(), textW).c_str(), !selected);
      renderer.drawText(UI_10_FONT_ID, textX, titleY + lineH * 2 + 4,
                        renderer.truncatedText(UI_10_FONT_ID, entry.progressLabel.c_str(), textW).c_str(), !selected);
      renderer.drawText(UI_10_FONT_ID, textX, titleY + lineH * 3 + 6,
                        renderer.truncatedText(UI_10_FONT_ID, entry.currentFormLabel.c_str(), textW).c_str(),
                        !selected);
      if (!entry.nextFormLabel.empty()) {
        renderer.drawText(UI_10_FONT_ID, textX, titleY + lineH * 4 + 8,
                          renderer.truncatedText(UI_10_FONT_ID, entry.nextFormLabel.c_str(), textW).c_str(), !selected);
      }

      const std::string slotLabel = std::to_string(i + 1);
      const int badgeSize = 20;
      const int badgeX = x + slotWidth - badgeSize - 8;
      const int badgeY = y + 8;
      if (selected) {
        renderer.fillRect(badgeX, badgeY, badgeSize, badgeSize, false);
        renderer.drawRect(badgeX, badgeY, badgeSize, badgeSize, true);
      } else {
        renderer.fillRect(badgeX, badgeY, badgeSize, badgeSize, true);
      }
      const int badgeTextX =
          badgeX + std::max(0, (badgeSize - renderer.getTextWidth(UI_10_FONT_ID, slotLabel.c_str())) / 2);
      renderer.drawText(UI_10_FONT_ID, badgeTextX, badgeY + 3, slotLabel.c_str(), selected);
    }
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, recentBooks.size(), selectorIndex,
        [this](int index) { return recentBooks[index].book.title; },
        [this](int index) { return recentBooks[index].book.author; },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].book.path); });
  }

  // Help text
  const auto labels = mappedInput.mapLabels("« Home", "Open", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
