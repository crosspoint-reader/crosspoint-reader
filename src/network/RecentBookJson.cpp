#include "network/RecentBookJson.h"

#include <ArduinoJson.h>

#include <cmath>

#include "util/PokemonBookDataStore.h"

namespace network {

void appendBookProgressJson(JsonObject target, const BookProgressDataStore::ProgressData& progress) {
  target["format"] = BookProgressDataStore::kindName(progress.kind);
  target["percent"] = std::round(progress.percent * 100.0f) / 100.0f;
  target["page"] = progress.page;
  target["pageCount"] = progress.pageCount;
  target["position"] = BookProgressDataStore::formatPositionLabel(progress);
  if (progress.spineIndex >= 0) {
    target["spineIndex"] = progress.spineIndex;
  }
}

String buildRecentBookJson(const RecentBook& book, const bool includePokemon) {
  JsonDocument doc;
  doc["path"] = book.path;
  doc["title"] = book.title;
  doc["author"] = book.author;
  doc["last_position"] = "";
  doc["last_opened"] = 0;
  doc["hasCover"] = !book.coverBmpPath.empty();

  BookProgressDataStore::ProgressData progress;
  if (BookProgressDataStore::loadProgress(book.path, progress)) {
    doc["last_position"] = BookProgressDataStore::formatPositionLabel(progress);
    appendBookProgressJson(doc["progress"].to<JsonObject>(), progress);
  } else {
    doc["progress"] = nullptr;
  }

  if (includePokemon) {
    JsonDocument pokemonDoc;
    if (PokemonBookDataStore::loadPokemonDocument(book.path, pokemonDoc) && pokemonDoc["pokemon"].is<JsonObject>()) {
      doc["pokemon"] = pokemonDoc["pokemon"];
    }
  }

  String output;
  serializeJson(doc, output);
  return output;
}

}  // namespace network
