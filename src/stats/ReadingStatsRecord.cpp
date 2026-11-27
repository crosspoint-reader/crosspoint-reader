#include "ReadingStatsRecord.h"

namespace readingstats {
namespace {

constexpr uint32_t TOTALS_MAGIC = 0x54545352;  // "RSTT"
constexpr uint32_t BOOK_MAGIC = 0x4B425352;    // "RSBK"
constexpr uint8_t VERSION = 1;

uint32_t addSaturating(const uint32_t a, const uint32_t b) { return a > UINT32_MAX - b ? UINT32_MAX : a + b; }

uint32_t crc32(const uint8_t* data, const size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

class Writer {
 public:
  explicit Writer(uint8_t* out) : out(out) {}
  void u8(const uint8_t v) { out[pos++] = v; }
  void u16(const uint16_t v) {
    u8(static_cast<uint8_t>(v));
    u8(static_cast<uint8_t>(v >> 8));
  }
  void u32(const uint32_t v) {
    u16(static_cast<uint16_t>(v));
    u16(static_cast<uint16_t>(v >> 16));
  }
  // Appends the CRC of everything written so far.
  void finish() { u32(crc32(out, pos)); }

 private:
  uint8_t* out;
  size_t pos = 0;
};

class Reader {
 public:
  explicit Reader(const uint8_t* in) : in(in) {}
  uint8_t u8() { return in[pos++]; }
  uint16_t u16() {
    const uint16_t lo = u8();
    return static_cast<uint16_t>(lo | (u8() << 8));
  }
  uint32_t u32() {
    const uint32_t lo = u16();
    return lo | (static_cast<uint32_t>(u16()) << 16);
  }

 private:
  const uint8_t* in;
  size_t pos = 0;
};

// Checks the size, trailing CRC, magic and version.
bool openRecord(const uint8_t* data, const size_t len, const size_t expected, const uint32_t magic) {
  if (!data || len != expected) return false;
  Reader crc(data + expected - 4);
  if (crc.u32() != crc32(data, expected - 4)) return false;
  Reader header(data);
  return header.u32() == magic && header.u8() == VERSION;
}

}  // namespace

void applySession(Totals& totals, Book& book, const uint32_t seconds, const uint32_t pages, const int32_t localDay) {
  totals.seconds = addSaturating(totals.seconds, seconds);
  totals.pages = addSaturating(totals.pages, pages);
  book.seconds = addSaturating(book.seconds, seconds);
  book.pages = addSaturating(book.pages, pages);
  if (seconds >= MIN_SESSION_SECONDS) {
    totals.sessions = addSaturating(totals.sessions, 1);
    totals.sessionSeconds = addSaturating(totals.sessionSeconds, seconds);
    book.sessions = addSaturating(book.sessions, 1);
    book.sessionSeconds = addSaturating(book.sessionSeconds, seconds);
  }
  // An unknown date, or one before the latest reading day (the clock moved
  // back), leaves the day counters as they were.
  if (seconds == 0 || localDay <= 0 || localDay < totals.lastReadDay) return;
  if (localDay == totals.lastReadDay) {
    totals.lastDaySeconds = addSaturating(totals.lastDaySeconds, seconds);
    return;
  }
  totals.lastDaySeconds = seconds;
  const bool continues = totals.lastReadDay > 0 && localDay == totals.lastReadDay + 1;
  totals.streakDays =
      continues ? static_cast<uint16_t>(totals.streakDays < UINT16_MAX ? totals.streakDays + 1 : UINT16_MAX) : 1;
  totals.lastReadDay = localDay;
}

bool setFinished(Totals& totals, Book& book, const bool finished) {
  if (book.finished == finished) return false;
  book.finished = finished;
  if (finished) {
    totals.finishedBooks = addSaturating(totals.finishedBooks, 1);
  } else if (totals.finishedBooks > 0) {
    totals.finishedBooks--;
  }
  return true;
}

uint16_t currentStreak(const Totals& totals, const int32_t today) {
  if (totals.lastReadDay <= 0 || today <= 0) return 0;
  const int32_t gap = today - totals.lastReadDay;
  return gap == 0 || gap == 1 ? totals.streakDays : 0;
}

uint32_t secondsOnDay(const Totals& totals, const int32_t day) {
  return day > 0 && day == totals.lastReadDay ? totals.lastDaySeconds : 0;
}

int32_t civilDay(int year, const int month, const int day) {
  // Howard Hinnant's days_from_civil.
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const int yoe = year - era * 400;
  const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void encode(const Totals& totals, uint8_t (&out)[TOTALS_BYTES]) {
  Writer w(out);
  w.u32(TOTALS_MAGIC);
  w.u8(VERSION);
  w.u32(totals.seconds);
  w.u32(totals.sessions);
  w.u32(totals.sessionSeconds);
  w.u32(totals.pages);
  w.u32(totals.finishedBooks);
  w.u32(static_cast<uint32_t>(totals.lastReadDay));
  w.u16(totals.streakDays);
  w.u32(totals.lastDaySeconds);
  w.finish();
}

void encode(const Book& book, uint8_t (&out)[BOOK_BYTES]) {
  Writer w(out);
  w.u32(BOOK_MAGIC);
  w.u8(VERSION);
  w.u32(book.seconds);
  w.u32(book.sessions);
  w.u32(book.sessionSeconds);
  w.u32(book.pages);
  w.u8(book.finished ? 1 : 0);
  w.finish();
}

bool decode(const uint8_t* data, const size_t len, Totals& out) {
  if (!openRecord(data, len, TOTALS_BYTES, TOTALS_MAGIC)) return false;
  Reader r(data + 5);
  out.seconds = r.u32();
  out.sessions = r.u32();
  out.sessionSeconds = r.u32();
  out.pages = r.u32();
  out.finishedBooks = r.u32();
  out.lastReadDay = static_cast<int32_t>(r.u32());
  out.streakDays = r.u16();
  out.lastDaySeconds = r.u32();
  return true;
}

bool decode(const uint8_t* data, const size_t len, Book& out) {
  if (!openRecord(data, len, BOOK_BYTES, BOOK_MAGIC)) return false;
  Reader r(data + 5);
  out.seconds = r.u32();
  out.sessions = r.u32();
  out.sessionSeconds = r.u32();
  out.pages = r.u32();
  out.finished = r.u8() != 0;
  return true;
}

}  // namespace readingstats
