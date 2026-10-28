#include "BookFontFile.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include "CrossPointSettings.h"

namespace {
std::string fontPath(const std::string& cacheDir) { return cacheDir + "/font.json"; }

// The keys match settings.json. Files from before spacing was kept per book
// hold only the family and size.
constexpr char FAMILY_KEY[] = "sdFontFamilyName";
constexpr char SIZE_KEY[] = "fontSize";
constexpr char LINE_KEY[] = "lineHeight";
constexpr char WORD_KEY[] = "wordSpacing";
constexpr char CHARACTER_KEY[] = "characterSpacing";

// Sets `value` and adds `field` when the file has `key` in [min, max].
void readField(const JsonDocument& doc, const char* key, const uint8_t min, const uint8_t max, uint8_t& value,
               BookFont& book, const uint8_t field) {
  if (!doc[key].is<uint8_t>()) return;
  const uint8_t v = doc[key].as<uint8_t>();
  if (v < min || v > max) return;
  value = v;
  book.fields |= field;
}
}  // namespace

void BookFontFile::load(const std::string& cacheDir, BookFont& book) {
  book = BookFont{};
  const std::string path = fontPath(cacheDir);
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path.c_str(), doc)) return;
  ReaderFont& font = book.font;
  readField(doc, SIZE_KEY, 1, UINT8_MAX, font.fontPointSize, book, BookFontField::Font);
  if (book.fields & BookFontField::Font) font.setFamily(doc[FAMILY_KEY] | "");
  readField(doc, LINE_KEY, 0, CrossPointSettings::LINE_COMPRESSION_COUNT - 1, font.lineSpacing, book,
            BookFontField::LineSpacing);
  readField(doc, WORD_KEY, CrossPointSettings::WORD_SPACING_MIN, CrossPointSettings::WORD_SPACING_MAX, font.wordSpacing,
            book, BookFontField::WordSpacing);
  readField(doc, CHARACTER_KEY, 0, 2 * CrossPointSettings::CHARACTER_SPACING_OFFSET, font.characterSpacing, book,
            BookFontField::CharacterSpacing);
}

bool BookFontFile::save(const std::string& cacheDir, const BookFont& book) {
  JsonDocument doc;
  const ReaderFont& font = book.font;
  if (book.fields & BookFontField::Font) {
    doc[FAMILY_KEY] = font.sdFontFamilyName;
    doc[SIZE_KEY] = font.fontPointSize;
  }
  if (book.fields & BookFontField::LineSpacing) doc[LINE_KEY] = font.lineSpacing;
  if (book.fields & BookFontField::WordSpacing) doc[WORD_KEY] = font.wordSpacing;
  if (book.fields & BookFontField::CharacterSpacing) doc[CHARACTER_KEY] = font.characterSpacing;
  const std::string path = fontPath(cacheDir);
  if (!PersistableStoreBase::writeDocToFile(path.c_str(), doc)) {
    LOG_ERR("BFNT", "Failed to save book font");
    return false;
  }
  return true;
}

bool BookFontFile::remove(const std::string& cacheDir) {
  const std::string path = fontPath(cacheDir);
  if (!Storage.exists(path.c_str()) || Storage.remove(path.c_str())) return true;
  LOG_ERR("BFNT", "Failed to remove book font");
  return false;
}
