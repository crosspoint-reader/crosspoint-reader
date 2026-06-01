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

// ─── Layout constants ─────────────────────────────────────────────────────────

// Left/right margin width; texture fills the 0..MARGIN_X and W-MARGIN_X..W strips.
static constexpr int MARGIN_X = 14;
// Clear white gap above and below each textured content band.
static constexpr int TEX_GAP = 8;
// Inner vertical padding inside the paper block (between band edge and text).
static constexpr int TEX_PADV = 4;
// Inner horizontal text indent inside the paper block.
static constexpr int TEX_PADH = 9;
// Extra left indent applied to item title text (e.g. "#01 Clean My Room"); the
// white paper block hugs the text (fit-content) and texture fills the leftover.
static constexpr int TITLE_INDENT = 8;
// Solid-black LEAVE BY / time banner height.
static constexpr int BAR_HEIGHT = 30;
// Tightened stale-data banner height.
static constexpr int STALE_BAR_H = 18;

// ─── Font assignments ─────────────────────────────────────────────────────────
// Extra-small halftone header; medium halftone titles at 16pt for visual weight.
static constexpr int HEADER_FONT = UI_10_FONT_ID;
static constexpr int TITLE_FONT = NOTOSANS_16_FONT_ID;
// Solid-black time banner: smaller font to fit "Tuesday 4:00PM  Leave in 2D 18H 12M".
static constexpr int BANNER_FONT = NOTOSANS_12_FONT_ID;
static constexpr int SUB_FONT = NOTOSANS_14_FONT_ID;
static constexpr int DETAIL_FONT = NOTOSANS_12_FONT_ID;
static constexpr int FOOTER_FONT = UI_10_FONT_ID;

// Sanity floor: 2023-11-14. Below this the clock is unset (deep-sleep wake
// resets newlib's clock), so countdowns and today's date are suppressed.
static constexpr time_t MIN_VALID_EPOCH = 1700000000;

static const char* const WDAY[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char* const WDAY_FULL[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char* const MON[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

// ─── Time helpers ─────────────────────────────────────────────────────────────

int localOffsetSecs() { return (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15 * 60; }

void localTm(time_t utc, struct tm& out) {
  const time_t local = utc + localOffsetSecs();
  gmtime_r(&local, &out);
}

void formatClock12(time_t epoch, char* buf, size_t len) {
  struct tm t;
  localTm(epoch, t);
  int h = t.tm_hour % 12;
  if (h == 0) h = 12;
  snprintf(buf, len, "%d:%02d%s", h, t.tm_min, t.tm_hour < 12 ? "am" : "pm");
}

// Uppercase 12-hour, e.g. "4:00PM" — used in the solid-black time banner.
void formatClock12Upper(time_t epoch, char* buf, size_t len) {
  char tmp[16];
  formatClock12(epoch, tmp, sizeof(tmp));
  size_t i = 0;
  for (; i < len - 1 && tmp[i]; i++)
    buf[i] = (tmp[i] >= 'a' && tmp[i] <= 'z') ? static_cast<char>(tmp[i] - 32) : tmp[i];
  buf[i] = '\0';
}

// Verbose countdown, e.g. "01H 56M" or "2D 18H 12M" once the gap spans a day.
void formatCountdownVerbose(long secsLeft, char* buf, size_t len) {
  if (secsLeft < 0) secsLeft = 0;
  const long days = secsLeft / 86400;
  const long hours = (secsLeft % 86400) / 3600;
  const long mins = (secsLeft % 3600) / 60;
  if (days > 0)
    snprintf(buf, len, "%ldD %02ldH %02ldM", days, hours, mins);
  else
    snprintf(buf, len, "%02ldH %02ldM", hours, mins);
}

// Compact HH:MM countdown — e.g. "01:23" for 1h 23m, "2D 18H" for multi-day.
void formatCountdownCompact(long secsLeft, char* buf, size_t len) {
  if (secsLeft < 0) secsLeft = 0;
  const long days = secsLeft / 86400;
  const long hours = (secsLeft % 86400) / 3600;
  const long mins = (secsLeft % 3600) / 60;
  if (days > 0)
    snprintf(buf, len, "%ldD %02ldH", days, hours);
  else
    snprintf(buf, len, "%02ld:%02ld", hours, mins);
}

// Uppercase 12-hour time, prefixed with the full weekday name when `epoch` falls
// on a different local day than `now` (e.g. "Tuesday 4:00PM" vs "4:00PM").
void formatTimeWithDay(time_t epoch, time_t now, bool clockValid, char* buf, size_t len) {
  char clk[16];
  formatClock12Upper(epoch, clk, sizeof(clk));
  if (clockValid) {
    struct tm et, nt;
    localTm(epoch, et);
    localTm(now, nt);
    if (et.tm_year != nt.tm_year || et.tm_yday != nt.tm_yday) {
      snprintf(buf, len, "%s %s", WDAY_FULL[et.tm_wday % 7], clk);
      return;
    }
  }
  snprintf(buf, len, "%s", clk);
}

void formatDateUtc(time_t epoch, char* buf, size_t len) {
  struct tm t;
  gmtime_r(&epoch, &t);
  snprintf(buf, len, "%s %s %d", WDAY[t.tm_wday % 7], MON[t.tm_mon % 12], t.tm_mday);
}

// Dotted line (every other pixel) — footer divider.
void dottedHLine(const GfxRenderer& r, int x, int y, int w) {
  for (int i = 0; i < w; i += 2) r.drawPixel(x + i, y, true);
}

// Top y so that a single text line is vertically centred in a band of height h.
int centeredTextTop(const GfxRenderer& r, int font, int bandTop, int h) {
  return bandTop + (h - r.getTextHeight(font)) / 2;
}

// ─── Texture engine ───────────────────────────────────────────────────────────
//
// Hierarchy (density encodes visual weight, highest → lowest):
//   Solid black banner  >  Dense halftone  >  Medium halftone  >
//   Crosshatch  >  Light halftone  >  Ruled lines  >  Dot grid

enum class Tex : uint8_t { Dense, Med, Cross, Light, Ruled, DotGrid };

// Returns true if pixel (px, py) should be drawn black for texture t.
bool texelAt(Tex t, int px, int py) {
  switch (t) {
    case Tex::Dense: {  // 8×8 tile, 2×2 dot (radius ≈ 1.44)
      const int tx = px & 7, ty = py & 7;
      return (tx == 3 || tx == 4) && (ty == 3 || ty == 4);
    }
    case Tex::Med: {  // 8×8 tile, 1×2 dot (radius ≈ 1.1)
      return (px & 7) == 4 && ((py & 7) == 3 || (py & 7) == 4);
    }
    case Tex::Cross: {  // 45° diagonal lines, 8px pitch
      return ((px + py) & 7) == 0;
    }
    case Tex::Light: {  // 8×8 tile, 1×1 dot (radius ≈ 0.85)
      return (px & 7) == 4 && (py & 7) == 4;
    }
    case Tex::Ruled: {  // 1px horizontal rule every 5px
      return py % 5 == 0;
    }
    case Tex::DotGrid: {  // 12×12 tile, 1×1 dot
      return px % 12 == 6 && py % 12 == 6;
    }
  }
  return false;
}

// Paints texture in the left [0, marginX) and right [W-marginX, W) strips for
// the horizontal band [bandY, bandY+bandH). The centre content area (already
// white from clearScreen) is intentionally untouched — this is the Android-style
// approach where the texture fills only the padding frame, not the text area.
void paintTex(const GfxRenderer& r, int bandY, int bandH, Tex tex, int W, int marginX) {
  const int bandEnd = bandY + bandH;
  for (int py = bandY; py < bandEnd; py++) {
    for (int px = 0; px < marginX; px++)
      if (texelAt(tex, px, py)) r.drawPixel(px, py, true);
    for (int px = W - marginX; px < W; px++)
      if (texelAt(tex, px, py)) r.drawPixel(px, py, true);
  }
}

// ─── Zone helpers ─────────────────────────────────────────────────────────────

// Total pixel height consumed by a zone with nLines text lines.
int zoneH(int lineH, int nLines = 1) { return TEX_GAP + nLines * lineH + TEX_PADV * 2 + TEX_GAP; }

// Draws a single-line textured zone and returns y after the zone.
// The texture fills only the left/right margin strips; the content block is white.
// xIndent shifts the text further right inside the content block.
int drawZone(const GfxRenderer& r, int font, const char* text, Tex tex, int y, int W, int contentLeft, int lineH,
             bool bold, bool center, int xIndent = 0) {
  const int paperH = lineH + TEX_PADV * 2;
  const int bandTop = y + TEX_GAP;
  paintTex(r, bandTop, paperH, tex, W, contentLeft);
  const int textY = bandTop + TEX_PADV;
  const auto fam = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  if (center) {
    r.drawCenteredText(font, textY, text, true, fam);
  } else {
    r.drawText(font, contentLeft + TEX_PADH + xIndent, textY, text, true, fam);
  }
  return y + TEX_GAP + paperH + TEX_GAP;
}

// Like drawZone, but the white "paper" block hugs the text (fit-content): the
// texture pattern is painted across the whole band and a white box just wide
// enough for the text (plus padding) is punched out around it, so the leftover
// space to the right of the text is filled with texture instead of white.
// `xIndent` shifts the text — and the white box's left edge — further right.
int drawZoneFit(const GfxRenderer& r, int font, const char* text, Tex tex, int y, int W, int contentLeft,
                int contentRight, int lineH, bool bold, int xIndent = 0) {
  const int paperH = lineH + TEX_PADV * 2;
  const int bandTop = y + TEX_GAP;
  const auto fam = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int textX = contentLeft + TEX_PADH + xIndent;
  const int textW = r.getTextWidth(font, text, fam);

  // White paper box: from the indented left edge to one padding past the text,
  // clamped to the content area so it never spills into the right margin.
  const int boxLeft = contentLeft + xIndent;
  int boxRight = textX + textW + TEX_PADH;
  if (boxRight > contentRight) boxRight = contentRight;

  // Texture the full band edge-to-edge first, then clear the white box so only
  // the leftover (right of the text) keeps the pattern.
  for (int py = bandTop; py < bandTop + paperH; py++)
    for (int px = 0; px < W; px++)
      if (texelAt(tex, px, py)) r.drawPixel(px, py, true);
  r.fillRect(boxLeft, bandTop, boxRight - boxLeft, paperH, false);

  r.drawText(font, textX, bandTop + TEX_PADV, text, true, fam);
  return y + TEX_GAP + paperH + TEX_GAP;
}

// Solid-black inverted time banner (highest visual weight). Returns y after.
int drawBanner(const GfxRenderer& r, const char* leftLabel, const char* rightLabel, int y, int W, int contentLeft,
               int contentRight, int contentWidth) {
  const int bannerY = y + TEX_GAP;
  r.fillRect(contentLeft, bannerY, contentWidth, BAR_HEIGHT, true);
  const int textTop = centeredTextTop(r, BANNER_FONT, bannerY, BAR_HEIGHT);
  r.drawText(BANNER_FONT, contentLeft + 8, textTop, leftLabel, false, EpdFontFamily::BOLD);
  if (rightLabel && rightLabel[0]) {
    const int rw = r.getTextWidth(BANNER_FONT, rightLabel, EpdFontFamily::BOLD);
    r.drawText(BANNER_FONT, contentRight - 8 - rw, textTop, rightLabel, false, EpdFontFamily::BOLD);
  }
  return y + TEX_GAP + BAR_HEIGHT + TEX_GAP;
}

// Stale-data bar (inverted, centred label).
void drawStaleBar(const GfxRenderer& r, const RemindersData& data, int barTop, int contentLeft, int contentWidth) {
  r.fillRect(contentLeft, barTop, contentWidth, STALE_BAR_H, true);
  char banner[56];
  if (data.synced_epoch > MIN_VALID_EPOCH) {
    char clk[16];
    formatClock12(data.synced_epoch, clk, sizeof(clk));
    snprintf(banner, sizeof(banner), "%s %s - %s", tr(STR_REMINDERS_LAST_SYNCED), clk, tr(STR_REMINDERS_NOT_LIVE));
  } else {
    snprintf(banner, sizeof(banner), "%s", tr(STR_REMINDERS_NOT_LIVE));
  }
  const int tw = r.getTextWidth(FOOTER_FONT, banner, EpdFontFamily::REGULAR);
  r.drawText(FOOTER_FONT, contentLeft + (contentWidth - tw) / 2, centeredTextTop(r, FOOTER_FONT, barTop, STALE_BAR_H),
             banner, false, EpdFontFamily::REGULAR);
}

// ─── Item block height (for pagination) ───────────────────────────────────────

int blockHeight(const CalItem& it, int titleH, int subH, int detailH) {
  (void)titleH;  // title is now a fixed-height solid black banner
  int h = TEX_GAP + BAR_HEIGHT + TEX_GAP;
  if (it.note_count > 0) h += it.note_count * zoneH(subH);
  if (it.start_epoch != 0) {
    if (it.all_day) {
      h += zoneH(detailH);
    } else {
      h += TEX_GAP + BAR_HEIGHT + TEX_GAP;
      if (it.is_calendar && it.travel_secs > 0) h += zoneH(detailH);
    }
  }
  if (it.location[0] != '\0') h += zoneH(detailH);
  return h;
}

// ─── Draw one item ────────────────────────────────────────────────────────────

int drawItem(const GfxRenderer& r, const CalItem& it, uint8_t number, int y, int W, int contentLeft, int contentRight,
             int contentWidth, time_t now, bool clockValid, int titleH, int subH, int detailH) {
  // ── Title — solid black banner, full width, white text ──────────────────
  {
    char buf[96];
    snprintf(buf, sizeof(buf), "#%02u  %s", number, it.title);
    const std::string trunc = r.truncatedText(TITLE_FONT, buf, contentWidth - 16);
    const int bannerY = y + TEX_GAP;
    r.fillRect(contentLeft, bannerY, contentWidth, BAR_HEIGHT, true);
    const int textTop = centeredTextTop(r, TITLE_FONT, bannerY, BAR_HEIGHT);
    r.drawText(TITLE_FONT, contentLeft + 8, textTop, trunc.c_str(), false, EpdFontFamily::BOLD);
    y = bannerY + BAR_HEIGHT + TEX_GAP;
  }

  // ── Sub-items — crosshatch, one zone per note with ↳ prefix ─────────────
  for (uint8_t n = 0; n < it.note_count; n++) {
    char noteBuf[60];
    snprintf(noteBuf, sizeof(noteBuf), "\xe2\x86\xb3 %.*s", static_cast<int>(sizeof(it.notes[0]) - 1), it.notes[n]);
    const std::string noteTrunc = r.truncatedText(SUB_FONT, noteBuf, contentWidth - TEX_PADH * 2 - 8);
    y = drawZone(r, SUB_FONT, noteTrunc.c_str(), Tex::Cross, y, W, contentLeft, subH, false, false, 8);
  }

  // ── Time / banner ────────────────────────────────────────────────────────
  if (it.start_epoch != 0) {
    if (it.all_day) {
      // All-day / dated task: plain date label, never a countdown.
      char dateBuf[24];
      formatDateUtc(it.start_epoch, dateBuf, sizeof(dateBuf));
      char line[48];
      snprintf(line, sizeof(line), "%s %s", it.is_calendar ? tr(STR_REMINDERS_ALL_DAY) : tr(STR_REMINDERS_DUE),
               dateBuf);
      y = drawZone(r, DETAIL_FONT, line, Tex::Ruled, y, W, contentLeft, detailH, false, false);
    } else {
      // Solid-black banner — highest visual weight.
      // Left: the actionable time (departure when travel is known, else start),
      // prefixed with the weekday when it isn't today. Right: "Leave in"/"Starts
      // in" plus a D/H/M countdown to that same time.
      const bool hasTravel = it.is_calendar && it.travel_secs > 0;
      const time_t countdownTarget = hasTravel ? it.start_epoch - it.travel_secs : it.start_epoch;
      char leftLabel[48], rightLabel[40];
      if (hasTravel) {
        char leaveBuf[16];
        formatClock12(countdownTarget, leaveBuf, sizeof(leaveBuf));
        snprintf(leftLabel, sizeof(leftLabel), "%s %s", tr(STR_REMINDERS_LEAVE_BY), leaveBuf);
      } else {
        formatTimeWithDay(it.start_epoch, now, clockValid, leftLabel, sizeof(leftLabel));
      }
      if (clockValid) {
        char cd[24];
        formatCountdownCompact(static_cast<long>(countdownTarget - now), cd, sizeof(cd));
        snprintf(rightLabel, sizeof(rightLabel), "%s %s", cd, tr(STR_REMINDERS_LEFT));
      } else {
        rightLabel[0] = '\0';
      }
      y = drawBanner(r, leftLabel, rightLabel, y, W, contentLeft, contentRight, contentWidth);

      // Meta row (event time + travel duration) — ruled lines.
      if (it.is_calendar && it.travel_secs > 0) {
        char metaBuf[64];
        char evtBuf[16];
        formatClock12(it.start_epoch, evtBuf, sizeof(evtBuf));
        snprintf(metaBuf, sizeof(metaBuf), "EVENT: %s  |  TRAVEL: %ld mins", evtBuf,
                 static_cast<long>(it.travel_secs / 60));
        const std::string metaTrunc = r.truncatedText(DETAIL_FONT, metaBuf, contentWidth - TEX_PADH * 2);
        y = drawZone(r, DETAIL_FONT, metaTrunc.c_str(), Tex::Ruled, y, W, contentLeft, detailH, false, false);
      }
    }
  }

  // ── Location — light halftone ────────────────────────────────────────────
  if (it.location[0] != '\0') {
    char dest[96];
    snprintf(dest, sizeof(dest), "%s %s", tr(STR_REMINDERS_DEST), it.location);
    const std::string destStr = r.truncatedText(DETAIL_FONT, dest, contentWidth - TEX_PADH * 2);
    y = drawZone(r, DETAIL_FONT, destStr.c_str(), Tex::Light, y, W, contentLeft, detailH, false, false);
  }

  return y;
}

}  // namespace

// ─── Public interface ─────────────────────────────────────────────────────────

uint8_t RemindersRenderer::drawLayout(GfxRenderer& renderer, const RemindersData& data, uint8_t startIndex) {
  // Force portrait — the 480×800 panel is always rendered portrait regardless
  // of what orientation the reader left the screen in.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();
  renderer.clearScreen(0xFF);  // white background

  const int contentLeft = MARGIN_X;
  const int contentRight = W - MARGIN_X;
  const int contentWidth = contentRight - contentLeft;

  const time_t now = time(nullptr);
  const bool clockValid = now > MIN_VALID_EPOCH;
  const time_t hdrEpoch = clockValid ? now : data.synced_epoch;

  const int titleH = renderer.getLineHeight(TITLE_FONT);
  const int subH = renderer.getLineHeight(SUB_FONT);
  const int detailH = renderer.getLineHeight(DETAIL_FONT);
  const int headerH = renderer.getLineHeight(HEADER_FONT);
  const int footerH = renderer.getLineHeight(FOOTER_FONT);

  // ── Header — dense halftone ──────────────────────────────────────────────
  int y = 0;
  {
    char header[80];
    if (hdrEpoch > MIN_VALID_EPOCH) {
      struct tm hdrTm;
      localTm(hdrEpoch, hdrTm);
      snprintf(header, sizeof(header), "%s  |  %s %s %d", tr(STR_REMINDERS_TASKS), WDAY[hdrTm.tm_wday % 7],
               MON[hdrTm.tm_mon % 12], hdrTm.tm_mday);
    } else {
      snprintf(header, sizeof(header), "%s", tr(STR_REMINDERS_TASKS));
    }
    y = drawZone(renderer, HEADER_FONT, header, Tex::Dense, y, W, contentLeft, headerH, false, true);
  }

  // ── Footer geometry — hints flush at the very bottom ────────────────────
  // No TOTAL counter or brand line: hints are the only footer element.
  const int hintHeight = 2 * footerH;
  const int hintTop = H - 6 - hintHeight;
  const int dividerY = hintTop - 6;

  int staleBarTop = 0;
  int contentBottom;
  if (data.is_stale) {
    staleBarTop = dividerY - 6 - STALE_BAR_H;
    contentBottom = staleBarTop - 4;
  } else {
    contentBottom = dividerY - 4;
  }

  if (data.count == 0) {
    renderer.drawCenteredText(TITLE_FONT, (y + contentBottom) / 2, tr(STR_REMINDERS_NO_TASKS));
  }

  // ── Item loop with pagination ────────────────────────────────────────────
  uint8_t i = startIndex;
  while (i < data.count) {
    const CalItem& it = data.items[i];
    const int bh = blockHeight(it, titleH, subH, detailH);
    if (i != startIndex && y + bh > contentBottom) break;
    y = drawItem(renderer, it, static_cast<uint8_t>(i + 1), y, W, contentLeft, contentRight, contentWidth, now,
                 clockValid, titleH, subH, detailH);
    i++;
  }
  const uint8_t nextIndex = i;
  const bool hasPrev = startIndex > 0;
  const bool hasMore = nextIndex < data.count;

  // ── Dot-grid texture fills remaining content space ───────────────────────
  if (y < contentBottom - TEX_GAP) {
    paintTex(renderer, y, contentBottom - y, Tex::DotGrid, W, contentLeft);
  }

  // ── Stale-data banner ────────────────────────────────────────────────────
  if (data.is_stale) {
    drawStaleBar(renderer, data, staleBarTop, contentLeft, contentWidth);
  }

  // ── Footer divider ───────────────────────────────────────────────────────
  dottedHLine(renderer, contentLeft, dividerY, contentWidth);

  // ── Hints — flush at the bottom ──────────────────────────────────────────
  // Page navigation is appended only when the list spans multiple pages.
  {
    std::string hint;
    if (hasPrev || hasMore) {
      hint += tr(STR_REMINDERS_HINT_PAGE);
      hint += "  ";
    }
    hint += tr(STR_REMINDERS_HINT_SYNC);
    hint += "  ";
    hint += tr(STR_REMINDERS_HINT_EXIT);
    int hy = hintTop;
    for (const auto& line : renderer.wrappedText(FOOTER_FONT, hint.c_str(), contentWidth, 2)) {
      renderer.drawCenteredText(FOOTER_FONT, hy, line.c_str());
      hy += footerH;
    }
  }

  return nextIndex;
}

uint8_t RemindersRenderer::renderFull(GfxRenderer& renderer, const RemindersData& data, uint8_t startIndex) {
  const uint8_t nextIndex = drawLayout(renderer, data, startIndex);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  return nextIndex;
}

bool RemindersRenderer::renderCountdownsOnly(GfxRenderer& renderer, const RemindersData& data, uint8_t startIndex) {
  drawLayout(renderer, data, startIndex);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  const time_t now = time(nullptr);
  for (uint8_t k = 0; k < data.count; k++) {
    const CalItem& it = data.items[k];
    if (!it.all_day && it.start_epoch != 0 && it.start_epoch <= now && it.start_epoch > data.synced_epoch) return false;
  }
  return true;
}
