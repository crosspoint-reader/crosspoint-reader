#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// The screen calibration card (Display → Screen calibration, Nikolai 05.11.2026): 33 gray steps from black to white,
// four corner marks and two paper cells, the layout the KEINKT Calibrator page measures (calibrator/index.html,
// renderCard). The pictures card is a BMP opened in the image viewer; the books card is a one-page EPUB, so the reader
// draws it the way it draws images inside books. Both are drawn through the current curve: with the checkbox off they
// show the panel's own response (calibrate), with it on the corrected one (check).
namespace calibcard {
constexpr int W = 480;
constexpr int H = 800;
constexpr char DIR[] = "/.crosspoint/calibration";

// Card tone at (x, y) of the portrait card. `books`: the last cell is black instead of paper — the page tells the cards
// apart by it.
uint8_t pixel(int x, int y, bool books);

// Writes the card (if missing) and returns its path: the pictures BMP in the reading orientation's shape, the books
// EPUB.
bool picturesCard(bool landscape, std::string& path);
bool booksCard(std::string& path);

// A file under DIR: viewers and the reader return to Settings from it, and it never enters Recent books.
bool isCalibrationPath(const std::string& path);

// Byte sinks for the writers (a file, or a CRC pass before the ZIP entry header). Public for the host test.
struct Sink {
  virtual void put(const uint8_t* data, size_t size) = 0;
  virtual ~Sink() = default;
};
uint32_t crc32(uint32_t crc, const uint8_t* data, size_t size);
// 8-bit grayscale PNG of the portrait card, stored (uncompressed) deflate blocks.
void writePng(Sink& out, bool books);
// The one-page EPUB (STORE ZIP) holding writePng(books = true); `title` is the book title.
void writeEpub(Sink& out, const std::string& title);
}  // namespace calibcard
