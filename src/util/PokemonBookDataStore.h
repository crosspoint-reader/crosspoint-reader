#pragma once

#include <ArduinoJson.h>

#include <string>

class PokemonBookDataStore {
 public:
  static bool supportsBookPath(const std::string& bookPath);
  static bool resolveCachePath(const std::string& bookPath, std::string& outCachePath);
  static std::string getPokemonDataPath(const std::string& bookPath);
  static bool loadPokemonDocument(const std::string& bookPath, JsonDocument& doc);
  static bool savePokemonDocument(const std::string& bookPath, JsonVariantConst pokemonData);
  static bool deletePokemonDocument(const std::string& bookPath);
};
