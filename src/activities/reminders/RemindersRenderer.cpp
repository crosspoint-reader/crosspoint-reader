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

int localOffsetSecs() { return (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15 * 60; }

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

// English weekday / month abbreviations. These are calendar-format tokens rather
// than UI chrome; kept ASCII to avoid a large per-locale string table.
const char* const WDAY[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char* const MON[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
}  // namespace

void RemindersRenderer::drawLayout(GfxRenderer& renderer, const RemindersData& data) {
  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();

  renderer.clearScreen(0xFF);  // white background

  // Outer dotted frame.
  dottedBorder(renderer, 4, 4, W - 8, H - 8);

  const int contentLeft = MARGIN_X;
  const int contentRight = W - MARGIN_X;
  const int contentWidth = contentRight - contentLeft;

  // --- Header: "TASKS | FRI, MAY 29 | N ITEMS" ---
  int y = 18 + renderer.getFontAscenderSize(HEADER_FONT);
  char header[64];
  const time_t now = time(nullptr);
  struct tm nowTm;
  localTm(now, nowTm);
  char itemsWord[24];
  snprintf(itemsWord, sizeof(itemsWord), "%u %s", data.count, tr(STR_REMINDERS_ITEMS));
  snprintf(header, sizeof(header), "%s | %s, %s %d | %s", tr(STR_REMINDERS_TASKS), WDAY[nowTm.tm_wday % 7],
           MON[nowTm.tm_mon % 12], nowTm.tm_mday, itemsWord);
  renderer.drawText(HEADER_FONT, contentLeft, y, header, true, EpdFontFamily::BOLD);
  y += 10;
  dottedHLine(renderer, contentLeft, y, contentWidth);
  y += renderer.getLineHeight(SUB_FONT);

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
    renderer.drawText(TITLE_FONT, contentLeft, y + renderer.getFontAscenderSize(TITLE_FONT), title.c_str());
    y += titleLine;

    // Sub-items (task notes), indented with a turnstile glyph.
    for (uint8_t n = 0; n < it.note_count; n++) {
      if (y > H - footerReserve - subLine) break;
      char noteBuf[80];
      snprintf(noteBuf, sizeof(noteBuf), "  > %s", it.notes[n]);
      const std::string note = renderer.truncatedText(SUB_FONT, noteBuf, contentWidth - 12);
      renderer.drawText(SUB_FONT, contentLeft + 12, y + renderer.getFontAscenderSize(SUB_FONT), note.c_str());
      y += subLine;
    }

    // Countdown bar for timed items: inverted black bar, white text.
    if (it.start_epoch != 0) {
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

      char cd[16];
      formatCountdown(static_cast<long>(it.start_epoch - now), cd, sizeof(cd));
      char rightLabel[24];
      snprintf(rightLabel, sizeof(rightLabel), "%s %s", cd, tr(STR_REMINDERS_LEFT));

      const int textY = barY + (BAR_HEIGHT + renderer.getFontAscenderSize(DETAIL_FONT)) / 2 - 1;
      renderer.drawText(DETAIL_FONT, contentLeft + 6, textY, leftLabel, false, EpdFontFamily::BOLD);
      const int rw = renderer.getTextWidth(DETAIL_FONT, rightLabel, EpdFontFamily::BOLD);
      renderer.drawText(DETAIL_FONT, contentRight - 6 - rw, textY, rightLabel, false, EpdFontFamily::BOLD);
      y += BAR_HEIGHT + 4;
    }

    // Destination row.
    if (it.location[0] != '\0') {
      if (y <= H - footerReserve - detailLine) {
        char dest[96];
        snprintf(dest, sizeof(dest), "%s %s", tr(STR_REMINDERS_DEST), it.location);
        const std::string destStr = renderer.truncatedText(DETAIL_FONT, dest, contentWidth);
        renderer.drawText(DETAIL_FONT, contentLeft, y + renderer.getFontAscenderSize(DETAIL_FONT), destStr.c_str());
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
  renderer.drawText(DETAIL_FONT, contentLeft, footerY + renderer.getFontAscenderSize(DETAIL_FONT), footer, true,
                    EpdFontFamily::BOLD);
  renderer.drawCenteredText(DETAIL_FONT, footerY + 18, tr(STR_REMINDERS_FOOTER));
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
  const int ty = barY + (barH + renderer.getFontAscenderSize(DETAIL_FONT)) / 2 - 1;
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
    if (data.items[i].start_epoch != 0 && data.items[i].start_epoch <= now &&
        data.items[i].start_epoch > data.synced_epoch) {
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
