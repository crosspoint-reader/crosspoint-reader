#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <memory>
#include <new>

#include "CalibrationCard.h"

// SD part of the calibration card: written once into DIR, reused afterwards (the viewer and the EPUB caches key on it).
// «_vN»: a changed card layout gets a new name, so an old card on the SD is never reused.
namespace calibcard {
namespace {
constexpr char PICTURES_PORTRAIT[] = "/.crosspoint/calibration/pictures_portrait_v2.bmp";
constexpr char PICTURES_LANDSCAPE[] = "/.crosspoint/calibration/pictures_landscape_v2.bmp";
// v2: the OPF identifier became a valid UUID (v1 carried a non-hex one).
constexpr char BOOKS[] = "/.crosspoint/calibration/books_card_v2.epub";

struct FileSink final : Sink {
  HalFile& file;
  bool ok = true;
  explicit FileSink(HalFile& f) : file(f) {}
  void put(const uint8_t* data, const size_t n) override {
    if (ok && file.write(data, n) != n) ok = false;
  }
};

// Writes through a temporary file and renames it in place, so a half-written card never stays behind.
// `fill` returns false when it could not produce the content.
template <typename Fn>
bool writeFile(const char* path, Fn&& fill) {
  if (!Storage.ensureDirectoryExists(DIR)) return false;
  const std::string tmp = std::string(path) + ".tmp";
  Storage.remove(tmp.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("CAL", tmp, file)) return false;
  FileSink sink(file);
  const bool filled = fill(sink);
  const bool closed = file.close();
  if (!filled || !sink.ok || !closed || !Storage.rename(tmp.c_str(), path)) {
    LOG_ERR("CAL", "Writing %s failed", path);
    Storage.remove(tmp.c_str());
    return false;
  }
  return true;
}

// 24-bit BMP, bottom-up; landscape = the portrait card turned 90° (the calibrator page finds any rotation).
bool writeBmp(Sink& out, const bool landscape) {
  const int w = landscape ? H : W, h = landscape ? W : H;
  const uint32_t row = static_cast<uint32_t>(w) * 3, size = 54 + row * static_cast<uint32_t>(h);
  uint8_t hdr[54] = {'B', 'M'};
  auto le32 = [&](int at, uint32_t v) {
    for (int k = 0; k < 4; ++k) hdr[at + k] = static_cast<uint8_t>(v >> (8 * k));
  };
  le32(2, size);
  le32(10, 54);
  le32(14, 40);
  le32(18, static_cast<uint32_t>(w));
  le32(22, static_cast<uint32_t>(h));
  hdr[26] = 1;
  hdr[28] = 24;
  le32(34, row * static_cast<uint32_t>(h));
  le32(38, 2835);
  le32(42, 2835);
  out.put(hdr, sizeof(hdr));
  // The widest row, on the heap only while writing (2.4 KB).
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[H * 3]);
  if (!buf) {
    LOG_ERR("CAL", "OOM: card row");
    return false;
  }
  uint8_t* line = buf.get();
  for (int y = h - 1; y >= 0; --y) {
    for (int x = 0; x < w; ++x) {
      // The card at 86 % with paper around it: its corner marks stay clear of the reader's black bezel in a photo
      // (the first real photo, 05.11.2026, lost a mark 12 px from the edge). The page finds the card at any scale.
      const int xp = landscape ? y : x, yp = landscape ? H - 1 - x : y;
      const int xc = W / 2 + (xp - W / 2) * 100 / 86, yc = H / 2 + (yp - H / 2) * 100 / 86;
      const uint8_t v = pixel(xc, yc, false);
      line[x * 3] = line[x * 3 + 1] = line[x * 3 + 2] = v;
    }
    out.put(line, row);
  }
  return true;
}
}  // namespace

bool picturesCard(const bool landscape, std::string& path) {
  path = landscape ? PICTURES_LANDSCAPE : PICTURES_PORTRAIT;
  // One card in the folder: the viewer pages through its siblings (also the cards of older layouts).
  Storage.remove(landscape ? PICTURES_PORTRAIT : PICTURES_LANDSCAPE);
  for (const char* old :
       {"/.crosspoint/calibration/pictures_portrait_v1.bmp", "/.crosspoint/calibration/pictures_landscape_v1.bmp",
        "/.crosspoint/calibration/pictures_portrait.bmp", "/.crosspoint/calibration/pictures_landscape.bmp"}) {
    Storage.remove(old);
  }
  if (Storage.exists(path.c_str())) return true;
  return writeFile(path.c_str(), [&](Sink& s) { return writeBmp(s, landscape); });
}

bool booksCard(std::string& path) {
  path = BOOKS;
  Storage.remove("/.crosspoint/calibration/books_card_v1.epub");  // an older layout is never reused.
  if (Storage.exists(BOOKS)) return true;
  const std::string title = tr(STR_CAL_BOOKS_SCREEN);
  return writeFile(BOOKS, [&](Sink& s) {
    writeEpub(s, title);
    return true;
  });
}

bool isCalibrationPath(const std::string& path) {
  static constexpr char PREFIX[] = "/.crosspoint/calibration/";
  return path.compare(0, sizeof(PREFIX) - 1, PREFIX) == 0;
}
}  // namespace calibcard
