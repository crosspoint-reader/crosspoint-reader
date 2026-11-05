#include "CalibrationCard.h"

#include <cstring>

// Pure part: the card, PNG and EPUB bytes (no SD access; host-tested in test/calibration_card).
namespace calibcard {
namespace {
// The layout of the KEINKT Calibrator page (calibrator/index.html, CORE): keep both in step.
constexpr int F = 56, HOLE = 24, FM = 12;
constexpr int COLS = 5, ROWS = 7, CELL = 72, GAP = 12;
constexpr int GX = (W - (COLS * CELL + (COLS - 1) * GAP)) / 2;  // 36
constexpr int GY = (H - (ROWS * CELL + (ROWS - 1) * GAP)) / 2;  // 112
constexpr int LEVELS = 33;
constexpr int FID[4][2] = {{FM + F / 2, FM + F / 2},
                           {W - FM - F / 2, FM + F / 2},
                           {W - FM - F / 2, H - FM - F / 2},
                           {FM + F / 2, H - FM - F / 2}};

void put16le(Sink& out, uint16_t v) {
  const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
  out.put(b, 2);
}
void put32le(Sink& out, uint32_t v) {
  const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                        static_cast<uint8_t>(v >> 24)};
  out.put(b, 4);
}
void put32be(Sink& out, uint32_t v) {
  const uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
                        static_cast<uint8_t>(v)};
  out.put(b, 4);
}
void putStr(Sink& out, const char* s) { out.put(reinterpret_cast<const uint8_t*>(s), strlen(s)); }

// Counts bytes and their CRC: the ZIP local header needs both before the data.
struct CrcSink final : Sink {
  uint32_t crc = 0;
  uint32_t size = 0;
  void put(const uint8_t* data, size_t n) override {
    crc = crc32(crc, data, n);
    size += static_cast<uint32_t>(n);
  }
};

// One PNG chunk: length, type, data, CRC over type + data.
struct Chunk {
  Sink& out;
  uint32_t crc;
  Chunk(Sink& o, uint32_t length, const char* type) : out(o) {
    put32be(out, length);
    out.put(reinterpret_cast<const uint8_t*>(type), 4);
    crc = crc32(0, reinterpret_cast<const uint8_t*>(type), 4);
  }
  void put(const uint8_t* data, size_t n) {
    out.put(data, n);
    crc = crc32(crc, data, n);
  }
  void end() { put32be(out, crc); }
};
}  // namespace

uint8_t pixel(const int x, const int y, const bool books) {
  if (x < 0 || y < 0 || x >= W || y >= H) return 255;
  for (const auto& f : FID) {
    const int dx = x - f[0], dy = y - f[1];
    if (dx >= -F / 2 && dx < F / 2 && dy >= -F / 2 && dy < F / 2) {
      const bool hole = dx >= -HOLE / 2 && dx < HOLE / 2 && dy >= -HOLE / 2 && dy < HOLE / 2;
      return hole ? 255 : 0;
    }
  }
  const int cx = x - GX, cy = y - GY;
  if (cx < 0 || cy < 0) return 255;
  const int col = cx / (CELL + GAP), row = cy / (CELL + GAP);
  if (col >= COLS || row >= ROWS || cx % (CELL + GAP) >= CELL || cy % (CELL + GAP) >= CELL) return 255;
  const int i = row * COLS + col;
  if (i < LEVELS) return static_cast<uint8_t>((2 * i * 255 + 32) / 64);  // round(i·255/32), as the page
  return (books && i == COLS * ROWS - 1) ? 0 : 255;
}

uint32_t crc32(uint32_t crc, const uint8_t* data, const size_t size) {
  static constexpr uint32_t T[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
                                     0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                     0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    crc = (crc >> 4) ^ T[crc & 15];
    crc = (crc >> 4) ^ T[crc & 15];
  }
  return ~crc;
}

void writePng(Sink& out, const bool books) {
  static constexpr uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  out.put(SIG, 8);
  {
    Chunk c(out, 13, "IHDR");
    const uint8_t ihdr[13] = {0, 0, W >> 8, W & 0xFF, 0, 0, H >> 8, H & 0xFF, 8, 0, 0, 0, 0};  // 8-bit gray
    c.put(ihdr, 13);
    c.end();
  }
  constexpr uint32_t ROW = W + 1;  // filter byte + pixels
  constexpr uint32_t RAW = ROW * H;
  constexpr uint32_t BLOCK = 65535;
  constexpr uint32_t BLOCKS = (RAW + BLOCK - 1) / BLOCK;
  Chunk c(out, 2 + BLOCKS * 5 + RAW + 4, "IDAT");
  static constexpr uint8_t ZHDR[2] = {0x78, 0x01};
  c.put(ZHDR, 2);
  uint32_t a = 1, b = 0;  // Adler-32 of the raw rows
  uint8_t buf[256];
  uint32_t pos = 0;
  while (pos < RAW) {
    const uint32_t len = RAW - pos < BLOCK ? RAW - pos : BLOCK;
    const uint8_t hdr[5] = {static_cast<uint8_t>(pos + len == RAW ? 1 : 0), static_cast<uint8_t>(len),
                            static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(~len),
                            static_cast<uint8_t>(~len >> 8)};
    c.put(hdr, 5);
    for (uint32_t done = 0; done < len;) {
      const uint32_t n = len - done < sizeof(buf) ? len - done : sizeof(buf);
      for (uint32_t k = 0; k < n; ++k) {
        const uint32_t idx = pos + done + k, row = idx / ROW, col = idx % ROW;
        buf[k] = col == 0 ? 0 : pixel(static_cast<int>(col - 1), static_cast<int>(row), books);
        a = (a + buf[k]) % 65521;
        b = (b + a) % 65521;
      }
      c.put(buf, n);
      done += n;
    }
    pos += len;
  }
  uint8_t adler[4] = {static_cast<uint8_t>(b >> 8), static_cast<uint8_t>(b), static_cast<uint8_t>(a >> 8),
                      static_cast<uint8_t>(a)};
  c.put(adler, 4);
  c.end();
  Chunk e(out, 0, "IEND");
  e.end();
}

void writeEpub(Sink& out, const std::string& title) {
  std::string t;
  for (const char ch : title) {
    if (ch == '&')
      t += "&amp;";
    else if (ch == '<')
      t += "&lt;";
    else if (ch == '>')
      t += "&gt;";
    else
      t += ch;
  }
  const std::string container =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<container version=\"1.0\" "
      "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
      "media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n";
  const std::string opf =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
      "unique-identifier=\"uid\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier "
      "id=\"uid\">urn:uuid:4b1f6c2e-9d0a-4f3e-8c51-keinkt-books-card</dc:identifier><dc:title>" +
      t +
      "</dc:title><dc:language>en</dc:language><meta property=\"dcterms:modified\">2026-11-05T00:00:00Z</meta>"
      "</metadata><manifest><item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
      "properties=\"nav\"/><item id=\"p1\" href=\"card.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"i1\" "
      "href=\"card.png\" media-type=\"image/png\"/></manifest><spine><itemref idref=\"p1\"/></spine></package>\n";
  const std::string nav =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" "
      "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><meta charset=\"utf-8\"/><title>" +
      t + "</title></head><body><nav epub:type=\"toc\"><ol><li><a href=\"card.xhtml\">" + t +
      "</a></li></ol></nav></body></html>\n";
  const std::string page =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
      "<head><meta charset=\"utf-8\"/><title>" +
      t + "</title></head><body><div><img src=\"card.png\" alt=\"\" style=\"width:100%\"/></div></body></html>\n";

  struct Entry {
    const char* name;
    const std::string* text;  // nullptr: the PNG
    uint32_t crc = 0, size = 0, offset = 0;
  };
  Entry entries[] = {{"mimetype", nullptr},       {"META-INF/container.xml", &container},
                     {"OEBPS/content.opf", &opf}, {"OEBPS/nav.xhtml", &nav},
                     {"OEBPS/card.xhtml", &page}, {"OEBPS/card.png", nullptr}};
  static const std::string MIMETYPE = "application/epub+zip";
  entries[0].text = &MIMETYPE;

  CrcSink png;
  writePng(png, /*books=*/true);

  struct Counter final : Sink {
    Sink& out;
    uint32_t pos = 0;
    explicit Counter(Sink& o) : out(o) {}
    void put(const uint8_t* d, size_t n) override {
      out.put(d, n);
      pos += static_cast<uint32_t>(n);
    }
  } w(out);

  for (auto& e : entries) {
    if (e.text) {
      e.crc = crc32(0, reinterpret_cast<const uint8_t*>(e.text->data()), e.text->size());
      e.size = static_cast<uint32_t>(e.text->size());
    } else {
      e.crc = png.crc;
      e.size = png.size;
    }
    e.offset = w.pos;
    put32le(w, 0x04034b50);
    put16le(w, 20);
    put16le(w, 0);       // flags
    put16le(w, 0);       // STORE
    put16le(w, 0);       // time
    put16le(w, 0x0021);  // 1980-01-01
    put32le(w, e.crc);
    put32le(w, e.size);
    put32le(w, e.size);
    put16le(w, static_cast<uint16_t>(strlen(e.name)));
    put16le(w, 0);
    putStr(w, e.name);
    if (e.text) {
      w.put(reinterpret_cast<const uint8_t*>(e.text->data()), e.text->size());
    } else {
      writePng(w, /*books=*/true);
    }
  }
  const uint32_t cdStart = w.pos;
  for (const auto& e : entries) {
    put32le(w, 0x02014b50);
    put16le(w, 20);
    put16le(w, 20);
    put16le(w, 0);
    put16le(w, 0);
    put16le(w, 0);
    put16le(w, 0x0021);
    put32le(w, e.crc);
    put32le(w, e.size);
    put32le(w, e.size);
    put16le(w, static_cast<uint16_t>(strlen(e.name)));
    put16le(w, 0);
    put16le(w, 0);
    put16le(w, 0);
    put16le(w, 0);
    put32le(w, 0);
    put32le(w, e.offset);
    putStr(w, e.name);
  }
  const uint32_t cdSize = w.pos - cdStart;
  constexpr uint16_t N = sizeof(entries) / sizeof(entries[0]);
  put32le(w, 0x06054b50);
  put16le(w, 0);
  put16le(w, 0);
  put16le(w, N);
  put16le(w, N);
  put32le(w, cdSize);
  put32le(w, cdStart);
  put16le(w, 0);
}

}  // namespace calibcard
