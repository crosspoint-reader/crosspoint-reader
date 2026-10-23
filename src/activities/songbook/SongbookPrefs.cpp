#include "SongbookPrefs.h"

void SongbookPrefs::ensureLoaded() {
  if (loaded) return;
  loaded = true;
  loadFromFile();
}

void SongbookPrefs::toJson(JsonDocument& doc) const { doc["instrument"] = songbook::instrumentCode(instrument); }

bool SongbookPrefs::fromJson(const JsonVariantConst doc) {
  const char* code = doc["instrument"] | "";
  // An unknown code (from a newer firmware, say) falls back to guitar.
  if (!songbook::parseInstrumentCode(code, instrument)) instrument = songbook::Instrument::Guitar;
  return true;
}
