#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include "ChordShapes.h"

// Songbook choices that last across songs and reboots. Main task only.
class SongbookPrefs : public PersistableStore<SongbookPrefs> {
  SongbookPrefs() = default;

  friend class PersistableStore<SongbookPrefs>;

 public:
  songbook::Instrument instrument = songbook::Instrument::Guitar;

  // Reads the file on the first call after boot; a missing file keeps the defaults.
  void ensureLoaded();

  static const char* getFilePath() { return "/.crosspoint/songbook.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

 private:
  bool loaded = false;
};

#define SONGBOOK_PREFS SongbookPrefs::getInstance()
