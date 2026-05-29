#include "RemindersRenderer.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {
constexpr int MARGIN_X = 14;
constexpr int HEADER_FONT = NOTOSANS_14_FONT_ID;
constexpr int TITLE_FONT = NOTOSANS_14_FONT_ID;
constexpr int SUB_FONT = NOTOSANS_12_FONT_ID;
constexpr int DETAIL_FONT = UI_10_FONT_ID;
constexpr int BAR_HEIGHT = 22;

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
}  // namespace

void RemindersRenderer::drawLayout(GfxRenderer& renderer, const RemindersData& data) {
  // The layout is designed for the 480x800 portrait panel; force it so entering
  // from a landscape reader (which leaves the renderer rotated) can't skew it.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();

  renderer.clearScreen(0xFF);  // white background

  // Outer dotted frame.
  dottedBorder(renderer, 4, 4, W - 8, H - 8);

  const int contentLeft = MARGIN_X;
  const int contentRight = W - MARGIN_X;
  const int contentWidth = contentRight - contentLeft;

  // When the clock is unset (e.g. on the sleep screen after a deep-sleep wake)
  // we still show the list, but suppress live countdowns / today's date.
  const time_t now = time(nullptr);
  const bool clockValid = now > MIN_VALID_EPOCH;
  const time_t headerEpoch = clockValid ? now : data.synced_epoch;

  // --- Header: "TASKS | FRI MAY 29 | N ITEMS" (date omitted if clock unknown) ---
  int y = 16;
  char header[64];
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
  renderer.drawText(HEADER_FONT, contentLeft,
                    centeredTextTop(renderer, HEADER_FONT, y, renderer.getLineHeight(HEADER_FONT)), header, true,
                    EpdFontFamily::BOLD);
  y += renderer.getLineHeight(HEADER_FONT);
  dottedHLine(renderer, contentLeft, y, contentWidth);
  y += 8;

  if (data.count == 0) {
    renderer.drawCenteredText(TITLE_FONT, H / 2, tr(STR_REMINDERS_NO_TASKS));
  }

  const int titleLine = renderer.getLineHeight(TITLE_FONT);
  const int subLine = renderer.getLineHeight(SUB_FONT);
  const int detailLine = renderer.getLineHeight(DETAIL_FONT);
  const int footerReserve = 70;

  for (uint8_t i = 0; i < data.count; i++) {
    const CalItem& it = data.items[i];

    // Stop if the next block would collide with the footer area.
    if (y > H - footerReserve - titleLine) break;

    // Numbered title (truncated to the content width).
    char titleBuf[96];
    snprintf(titleBuf, sizeof(titleBuf), "#%02d %s", i + 1, it.title);
    const std::string title = renderer.truncatedText(TITLE_FONT, titleBuf, contentWidth);
    renderer.drawText(TITLE_FONT, contentLeft, y, title.c_str());
    y += titleLine;

    // Sub-items (task notes), indented with a turnstile glyph.
    for (uint8_t n = 0; n < it.note_count; n++) {
      if (y > H - footerReserve - subLine) break;
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
      snprintf(line, sizeof(line), "%s %s", it.is_calendar ? tr(STR_REMINDERS_ALL_DAY) : tr(STR_REMINDERS_DUE),
               dateBuf);
      renderer.drawText(DETAIL_FONT, contentLeft, y, line, true, EpdFontFamily::BOLD);
      y += detailLine;
    } else if (it.start_epoch != 0) {
      // Timed item: inverted black bar with white text.
      const int barY = y;
      renderer.fillRect(contentLeft, barY, contentWidth, BAR_HEIGHT, true);

      char clockBuf[16];
      formatClock12(it.start_epoch, clockBuf, sizeof(clockBuf));
      char leftLabel[40];
      // Calendar events with travel time say "LEAVE BY"; everything else shows
      // the start time directly.
      if (it.is_calendar && it.travel_secs > 0) {
        char depBuf[16];
        formatClock12(it.start_epoch - it.travel_secs, depBuf, sizeof(depBuf));
        snprintf(leftLabel, sizeof(leftLabel), "%s %s", tr(STR_REMINDERS_LEAVE_BY), depBuf);
      } else {
        snprintf(leftLabel, sizeof(leftLabel), "%s", clockBuf);
      }

      const int textTop = centeredTextTop(renderer, DETAIL_FONT, barY, BAR_HEIGHT);
      renderer.drawText(DETAIL_FONT, contentLeft + 6, textTop, leftLabel, false, EpdFontFamily::BOLD);
      // The countdown is only meaningful with a valid clock; otherwise show just
      // the start time on the left and no "LEFT" figure.
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
      if (y <= H - footerReserve - detailLine) {
        char dest[96];
        snprintf(dest, sizeof(dest), "%s %s", tr(STR_REMINDERS_DEST), it.location);
        const std::string destStr = renderer.truncatedText(DETAIL_FONT, dest, contentWidth);
        renderer.drawText(DETAIL_FONT, contentLeft, y, destStr.c_str());
        y += detailLine;
      }
    }

    // Per-item dotted divider.
    y += 4;
    dottedHLine(renderer, contentLeft, y, contentWidth);
    y += subLine;
  }

  // --- Footer ---
  const int footerY = H - 44;
  dottedHLine(renderer, contentLeft, footerY - 6, contentWidth);
  char footer[48];
  snprintf(footer, sizeof(footer), "%s %u", tr(STR_REMINDERS_TOTAL), data.count);
  renderer.drawText(DETAIL_FONT, contentLeft, footerY, footer, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(DETAIL_FONT, footerY + detailLine, tr(STR_REMINDERS_FOOTER));
}

void RemindersRenderer::drawStaleOverlay(GfxRenderer& renderer, const RemindersData& data) {
  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();
  const int barH = 24;
  const int barY = H - 44 - barH - 4;

  renderer.fillRect(MARGIN_X, barY, W - 2 * MARGIN_X, barH, true);

  char banner[48];
  if (data.synced_epoch != 0) {
    char clk[16];
    formatClock12(data.synced_epoch, clk, sizeof(clk));
    snprintf(banner, sizeof(banner), "%s %s - %s", tr(STR_REMINDERS_LAST_SYNCED), clk, tr(STR_REMINDERS_NOT_LIVE));
  } else {
    snprintf(banner, sizeof(banner), "%s", tr(STR_REMINDERS_NOT_LIVE));
  }
  const int tw = renderer.getTextWidth(DETAIL_FONT, banner, EpdFontFamily::BOLD);
  const int tx = (W - tw) / 2;
  const int ty = centeredTextTop(renderer, DETAIL_FONT, barY, barH);
  renderer.drawText(DETAIL_FONT, tx, ty, banner, false, EpdFontFamily::BOLD);
}

void RemindersRenderer::renderFull(GfxRenderer& renderer, const RemindersData& data) {
  drawLayout(renderer, data);
  if (data.is_stale) drawStaleOverlay(renderer, data);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

bool RemindersRenderer::renderCountdownsOnly(GfxRenderer& renderer, const RemindersData& data) {
  // On a 1-bit full-frame panel there is no partial-region update, so rebuild
  // the whole frame but present it with the flash-free FAST_REFRESH waveform.
  drawLayout(renderer, data);
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

void RemindersRenderer::renderStaleBanner(GfxRenderer& renderer, const RemindersData& data) {
  drawLayout(renderer, data);
  drawStaleOverlay(renderer, data);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
