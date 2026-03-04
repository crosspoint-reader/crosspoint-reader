#include "util/PokemonBookDataStore.h"

#include <HalStorage.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <memory>

namespace {
constexpr char kCacheBasePath[] = "/.crosspoint";
constexpr char kPokemonDataFileName[] = "/pokemon.json";

bool hasExtension(const std::string& bookPath, const char* extension) {
  const size_t extLen = std::strlen(extension);
  if (bookPath.size() < extLen) {
    return false;
  }

  const size_t start = bookPath.size() - extLen;
  for (size_t i = 0; i < extLen; ++i) {
    const unsigned char lhs = static_cast<unsigned char>(bookPath[start + i]);
    const unsigned char rhs = static_cast<unsigned char>(extension[i]);
    if (std::tolower(lhs) != std::tolower(rhs)) {
      return false;
    }
  }

  return true;
}

const char* cachePrefixForBookPath(const std::string& bookPath) {
  if (hasExtension(bookPath, ".epub")) {
    return "epub_";
  }
  if (hasExtension(bookPath, ".txt")) {
    return "txt_";
  }
  if (hasExtension(bookPath, ".md")) {
    return "md_";
  }
  if (hasExtension(bookPath, ".xtc") || hasExtension(bookPath, ".xtch")) {
    return "xtc_";
  }
  return nullptr;
}
}  // namespace

bool PokemonBookDataStore::supportsBookPath(const std::string& bookPath) {
  return cachePrefixForBookPath(bookPath) != nullptr;
}

bool PokemonBookDataStore::resolveCachePath(const std::string& bookPath, std::string& outCachePath) {
  const char* prefix = cachePrefixForBookPath(bookPath);
  if (prefix == nullptr) {
    outCachePath.clear();
    return false;
  }

  outCachePath = std::string(kCacheBasePath) + "/" + prefix + std::to_string(std::hash<std::string>{}(bookPath));
  return true;
}

std::string PokemonBookDataStore::getPokemonDataPath(const std::string& bookPath) {
  std::string cachePath;
  if (!resolveCachePath(bookPath, cachePath)) {
    return "";
  }
  return cachePath + kPokemonDataFileName;
}

bool PokemonBookDataStore::loadPokemonDocument(const std::string& bookPath, JsonDocument& doc) {
  const std::string pokemonDataPath = getPokemonDataPath(bookPath);
  if (pokemonDataPath.empty() || !Storage.exists(pokemonDataPath.c_str())) {
    return false;
  }

  const String json = Storage.readFile(pokemonDataPath.c_str());
  if (json.isEmpty()) {
    return false;
  }

  return !deserializeJson(doc, json.c_str());
}

bool PokemonBookDataStore::savePokemonDocument(const std::string& bookPath, JsonVariantConst pokemonData) {
  std::string cachePath;
  if (!resolveCachePath(bookPath, cachePath)) {
    return false;
  }

  Storage.mkdir(kCacheBasePath);
  Storage.mkdir(cachePath.c_str());

  JsonDocument doc;
  doc["pokemon"].set(pokemonData);

  const size_t jsonSize = measureJson(doc);
  std::unique_ptr<char[]> jsonBuffer(new char[jsonSize + 1]);
  serializeJson(doc, jsonBuffer.get(), jsonSize + 1);
  return Storage.writeFile((cachePath + kPokemonDataFileName).c_str(), jsonBuffer.get());
}

bool PokemonBookDataStore::deletePokemonDocument(const std::string& bookPath) {
  const std::string pokemonDataPath = getPokemonDataPath(bookPath);
  if (pokemonDataPath.empty()) {
    return false;
  }

  if (!Storage.exists(pokemonDataPath.c_str())) {
    return true;
  }

  return Storage.remove(pokemonDataPath.c_str());
}
