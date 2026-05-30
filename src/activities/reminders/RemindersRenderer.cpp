#include "RemindersRenderer.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {
constexpr int MARGIN_X = 14;
constexpr int HEADER_FONT = NOTOSANS_14_FONT_ID;
constexpr int TITLE_FONT = NOTOSANS_14_FONT_ID;
constexpr int SUB_FONT = NOTOSANS_12_FONT_ID;
constexpr int DETAIL_FONT = UI_10_FONT_ID;
constexpr int BAR_HEIGHT = 22;
constexpr int STALE_BAR_H = 18;  // tightened banner height
constexpr int FOOTER_HINT_LINES = 2;

// Sanity floor for the system clock: 2023-11-14. Below this the clock is unset
// (deep-sleep wake resets it), so countdowns are meaningless and are suppressed.
constexpr time_t MIN_VALID_EPOCH = 1700000000;

// English weekday / month abbreviations. These are calendar-format tokens rather
// than UI chrome; kept ASCII to avoid a large per-locale string table.
const char* const WDAY[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char* const MON[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

int localOffsetSecs() { return (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15 * 60; }

// Top y for vertically centering one text line within a band of height `h`.
// drawText() takes the cell top (it adds the ascender internally), and
// getTextHeight() returns that ascender, so this centers the glyph cell.
int centeredTextTop(const GfxRenderer& r, int font, int bandTop, int h) {
  return bandTop + (h - r.getTextHeight(font)) / 2;
}

// Local civil time for a UTC epoch (system TZ is UTC0; we apply the user's
// configured offset manually, same field the status-bar clock uses).
void localTm(time_t utc, struct tm& out) {
  const time_t local = utc + localOffsetSecs();
  gmtime_r(&local, &out);
}

// "6:38pm" / "12:00am" — 12-hour with a lowercase suffix.
void formatClock12(time_t epoch, char* buf, size_t len) {
  struct tm t;
  localTm(epoch, t);
  int h = t.tm_hour % 12;
  if (h == 0) h = 12;
  snprintf(buf, len, "%d:%02d%s", h, t.tm_min, t.tm_hour < 12 ? "am" : "pm");
}

// "01:18" (HH:MM) remaining; clamps negatives to 00:00.
void formatCountdown(long secsLeft, char* buf, size_t len) {
  if (secsLeft < 0) secsLeft = 0;
  const long h = secsLeft / 3600;
  const long m = (secsLeft % 3600) / 60;
  snprintf(buf, len, "%02ld:%02ld", h, m);
}

// "FRI MAY 29" from a date stored as that day's UTC midnight (no local offset —
// all-day dates are calendar dates, not instants).
void formatDateUtc(time_t epoch, char* buf, size_t len) {
  struct tm t;
  gmtime_r(&epoch, &t);
  snprintf(buf, len, "%s %s %d", WDAY[t.tm_wday % 7], MON[t.tm_mon % 12], t.tm_mday);
}

// Dotted horizontal line (every other pixel) — the border/divider motif.
void dottedHLine(const GfxRenderer& r, int x, int y, int w) {
  for (int i = 0; i < w; i += 2) r.drawPixel(x + i, y, true);
}

void dottedVLine(const GfxRenderer& r, int x, int y, int h) {
  for (int i = 0; i < h; i += 2) r.drawPixel(x, y + i, true);
}

void dottedBorder(const GfxRenderer& r, int x, int y, int w, int h) {
  dottedHLine(r, x, y, w);
  dottedHLine(r, x, y + h - 1, w);
  dottedVLine(r, x, y, h);
  dottedVLine(r, x + w - 1, y, h);
}

// Pixel height of one item block, matching exactly what drawItem() advances.
int blockHeight(const CalItem& it, int titleLine, int subLine, int detailLine) {
  int h = titleLine;
  h += it.note_count * subLine;
  if (it.start_epoch != 0) h += it.all_day ? detailLine : (BAR_HEIGHT + 4);
  if (it.location[0] != '\0') h += detailLine;
  h += 4 + subLine;  // per-item dotted divider + spacing
  return h;
}

// Draw one item starting at top `y`; returns the new y below the block.
int drawItem(GfxRenderer& renderer, const CalItem& it, uint8_t number, int y, int contentLeft, int contentRight,
             int contentWidth, time_t now, bool clockValid, int titleLine, int subLine, int detailLine) {
  // Numbered title.
  char titleBuf[96];
  snprintf(titleBuf, sizeof(titleBuf), "#%02u %s", number, it.title);
  const std::string title = renderer.truncatedText(TITLE_FONT, titleBuf, contentWidth);
  renderer.drawText(TITLE_FONT, contentLeft, y, title.c_str());
  y += titleLine;

  // Sub-items (task notes), indented with a turnstile glyph.
  for (uint8_t n = 0; n < it.note_count; n++) {
    char noteBuf[80];
    snprintf(noteBuf, sizeof(noteBuf), "  > %s", it.notes[n]);
    const std::string note = renderer.truncatedText(SUB_FONT, noteBuf, contentWidth - 12);
    renderer.drawText(SUB_FONT, contentLeft + 12, y, note.c_str());
    y += subLine;
  }

  if (it.all_day && it.start_epoch != 0) {
    // All-day event / dated task: a plain date label, never a countdown.
    char dateBuf[24];
    formatDateUtc(it.start_epoch, dateBuf, sizeof(dateBuf));
    char line[48];
    snprintf(line, sizeof(line), "%s %s", it.is_calendar ? tr(STR_REMINDERS_ALL_DAY) : tr(STR_REMINDERS_DUE), dateBuf);
    renderer.drawText(DETAIL_FONT, contentLeft, y, line, true, EpdFontFamily::BOLD);
    y += detailLine;
  } else if (it.start_epoch != 0) {
    // Timed item: inverted black bar with white text.
    const int barY = y;
    renderer.fillRect(contentLeft, barY, contentWidth, BAR_HEIGHT, true);

    char clockBuf[16];
    formatClock12(it.start_epoch, clockBuf, sizeof(clockBuf));
    char leftLabel[40];
    // Calendar events with travel time say "LEAVE BY"; everything else shows the start time.
    if (it.is_calendar && it.travel_secs > 0) {
      char depBuf[16];
      formatClock12(it.start_epoch - it.travel_secs, depBuf, sizeof(depBuf));
      snprintf(leftLabel, sizeof(leftLabel), "%s %s", tr(STR_REMINDERS_LEAVE_BY), depBuf);
    } else {
      snprintf(leftLabel, sizeof(leftLabel), "%s", clockBuf);
    }

    const int textTop = centeredTextTop(renderer, DETAIL_FONT, barY, BAR_HEIGHT);
    renderer.drawText(DETAIL_FONT, contentLeft + 6, textTop, leftLabel, false, EpdFontFamily::BOLD);
    // The countdown is only meaningful with a valid clock.
    if (clockValid) {
      char cd[16];
      formatCountdown(static_cast<long>(it.start_epoch - now), cd, sizeof(cd));
      char rightLabel[24];
      snprintf(rightLabel, sizeof(rightLabel), "%s %s", cd, tr(STR_REMINDERS_LEFT));
      const int rw = renderer.getTextWidth(DETAIL_FONT, rightLabel, EpdFontFamily::BOLD);
      renderer.drawText(DETAIL_FONT, contentRight - 6 - rw, textTop, rightLabel, false, EpdFontFamily::BOLD);
    }
    y += BAR_HEIGHT + 4;
  }

  // Destination row.
  if (it.location[0] != '\0') {
    char dest[96];
    snprintf(dest, sizeof(dest), "%s %s", tr(STR_REMINDERS_DEST), it.location);
    const std::string destStr = renderer.truncatedText(DETAIL_FONT, dest, contentWidth);
    renderer.drawText(DETAIL_FONT, contentLeft, y, destStr.c_str());
    y += detailLine;
  }

  // Per-item dotted divider.
  y += 4;
  dottedHLine(renderer, contentLeft, y, contentWidth);
  y += subLine;
  return y;
}

// Draw the tightened stale banner (inverted bar, white text) at `barTop`.
void drawStaleBar(GfxRenderer& renderer, const RemindersData& data, int barTop, int contentLeft, int contentWidth) {
  renderer.fillRect(contentLeft, barTop, contentWidth, STALE_BAR_H, true);
  char banner[48];
  if (data.synced_epoch > MIN_VALID_EPOCH) {
    char clk[16];
    formatClock12(data.synced_epoch, clk, sizeof(clk));
    snprintf(banner, sizeof(banner), "%s %s - %s", tr(STR_REMINDERS_LAST_SYNCED), clk, tr(STR_REMINDERS_NOT_LIVE));
  } else {
    snprintf(banner, sizeof(banner), "%s", tr(STR_REMINDERS_NOT_LIVE));
  }
  const int tw = renderer.getTextWidth(DETAIL_FONT, banner, EpdFontFamily::BOLD);
  renderer.drawText(DETAIL_FONT, contentLeft + (contentWidth - tw) / 2,
                    centeredTextTop(renderer, DETAIL_FONT, barTop, STALE_BAR_H), banner, false, EpdFontFamily::BOLD);
}
}  // namespace

uint8_t RemindersRenderer::drawLayout(GfxRenderer& renderer, const RemindersData& data, uint8_t startIndex) {
  // The layout is designed for the 480x800 portrait panel; force it so entering
  // from a landscape reader (which leaves the renderer rotated) can't skew it.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();

  renderer.clearScreen(0xFF);  // white background
  dottedBorder(renderer, 4, 4, W - 8, H - 8);

  const int contentLeft = MARGIN_X;
  const int contentRight = W - MARGIN_X;
  const int contentWidth = contentRight - contentLeft;

  // When the clock is unset (e.g. on the sleep screen after a deep-sleep wake)
  // we still show the list, but suppress live countdowns / today's date.
  const time_t now = time(nullptr);
  const bool clockValid = now > MIN_VALID_EPOCH;
  const time_t headerEpoch = clockValid ? now : data.synced_epoch;

  const int titleLine = renderer.getLineHeight(TITLE_FONT);
  const int subLine = renderer.getLineHeight(SUB_FONT);
  const int detailLine = renderer.getLineHeight(DETAIL_FONT);
  const int headerLine = renderer.getLineHeight(HEADER_FONT);

  // --- Header: "TASKS | FRI MAY 29 | N ITEMS", wrapped to two lines (never truncated mid-screen). ---
  int y = 16;
  char header[80];
  char itemsWord[24];
  snprintf(itemsWord, sizeof(itemsWord), "%u %s", data.count, tr(STR_REMINDERS_ITEMS));
  if (headerEpoch > MIN_VALID_EPOCH) {
    struct tm hdrTm;
    localTm(headerEpoch, hdrTm);
    snprintf(header, sizeof(header), "%s | %s %s %d | %s", tr(STR_REMINDERS_TASKS), WDAY[hdrTm.tm_wday % 7],
             MON[hdrTm.tm_mon % 12], hdrTm.tm_mday, itemsWord);
  } else {
    snprintf(header, sizeof(header), "%s | %s", tr(STR_REMINDERS_TASKS), itemsWord);
  }
  for (const auto& line : renderer.wrappedText(HEADER_FONT, header, contentWidth, 2, EpdFontFamily::BOLD)) {
    renderer.drawText(HEADER_FONT, contentLeft, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += headerLine;
  }
  y += 4;
  dottedHLine(renderer, contentLeft, y, contentWidth);
  y += 8;

  // --- Footer geometry (computed up front so the item loop knows its bottom). ---
  const int brandTop = H - 6 - detailLine;
  const int infoTop = brandTop - detailLine;
  const int hintTop = infoTop - FOOTER_HINT_LINES * detailLine;
  const int footerDividerY = hintTop - 8;
  int staleBarTop = 0;
  int contentBottom;
  if (data.is_stale) {
    staleBarTop = footerDividerY - 6 - STALE_BAR_H;
    contentBottom = staleBarTop - 6;
  } else {
    contentBottom = footerDividerY - 6;
  }

  if (data.count == 0) {
    renderer.drawCenteredText(TITLE_FONT, (y + contentBottom) / 2, tr(STR_REMINDERS_NO_TASKS));
  }

  // --- Item loop with pagination. Always draw the first item of the page, then
  // stop before any item that would overflow the content area. ---
  uint8_t i = startIndex;
  while (i < data.count) {
    const CalItem& it = data.items[i];
    const int h = blockHeight(it, titleLine, subLine, detailLine);
    if (i != startIndex && y + h > contentBottom) break;
    y = drawItem(renderer, it, static_cast<uint8_t>(i + 1), y, contentLeft, contentRight, contentWidth, now, clockValid,
                 titleLine, subLine, detailLine);
    i++;
  }
  const uint8_t nextIndex = i;
  const bool hasPrev = startIndex > 0;
  const bool hasMore = nextIndex < data.count;

  // --- Stale banner (folded into the layout so it tracks the footer geometry). ---
  if (data.is_stale) {
    drawStaleBar(renderer, data, staleBarTop, contentLeft, contentWidth);
  }

  // --- Footer ---
  dottedHLine(renderer, contentLeft, footerDividerY, contentWidth);

  // Controls hint (wrapped to two lines). Page nav is mentioned only when paging.
  std::string hint;
  if (hasPrev || hasMore) {
    hint += tr(STR_REMINDERS_HINT_PAGE);
    hint += "  ";
  }
  hint += tr(STR_REMINDERS_HINT_SYNC);
  hint += "  ";
  hint += tr(STR_REMINDERS_HINT_EXIT);
  int hy = hintTop;
  for (const auto& line : renderer.wrappedText(DETAIL_FONT, hint.c_str(), contentWidth, FOOTER_HINT_LINES)) {
    renderer.drawCenteredText(DETAIL_FONT, hy, line.c_str());
    hy += detailLine;
  }

  // Info line: total when everything fits on one page, otherwise the visible range.
  char info[40];
  if (startIndex == 0 && !hasMore) {
    snprintf(info, sizeof(info), "%s %u", tr(STR_REMINDERS_TOTAL), data.count);
  } else {
    snprintf(info, sizeof(info), "%u-%u / %u", startIndex + 1, nextIndex, data.count);
  }
  renderer.drawText(DETAIL_FONT, contentLeft, infoTop, info, true, EpdFontFamily::BOLD);

  // Brand line, device-aware (X3 / X4).
  char brand[48];
  snprintf(brand, sizeof(brand), "* %s %s *", tr(STR_REMINDERS_FOOTER), gpio.deviceIsX3() ? "X3" : "X4");
  renderer.drawCenteredText(DETAIL_FONT, brandTop, brand);

  return nextIndex;
}

uint8_t RemindersRenderer::renderFull(GfxRenderer& renderer, const RemindersData& data, uint8_t startIndex) {
  const uint8_t nextIndex = drawLayout(renderer, data, startIndex);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  return nextIndex;
}

bool RemindersRenderer::renderCountdownsOnly(GfxRenderer& renderer, const RemindersData& data, uint8_t startIndex) {
  // On a 1-bit full-frame panel there is no partial-region update, so rebuild
  // the whole frame but present it with the flash-free FAST_REFRESH waveform.
  drawLayout(renderer, data, startIndex);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  // Signal a full refresh is warranted if any timed item has already started:
  // its countdown just hit zero and FAST_REFRESH can leave ghosting.
  const time_t now = time(nullptr);
  for (uint8_t i = 0; i < data.count; i++) {
    const CalItem& it = data.items[i];
    if (!it.all_day && it.start_epoch != 0 && it.start_epoch <= now && it.start_epoch > data.synced_epoch) {
      return false;
    }
  }
  return true;
}
