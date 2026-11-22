from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
def source(relative):
    return (root / relative).read_text(encoding="utf-8")

base = source("src/components/themes/BaseTheme.cpp")
metrics = source("src/components/themes/BaseTheme.h")
rounded = source("src/components/themes/roundedraff/RoundedRaffTheme.cpp")
rounded_metrics = source("src/components/themes/roundedraff/RoundedRaffTheme.h")
lyra_metrics = source("src/components/themes/lyra/LyraTheme.h")

# The system header's clock/battery location is independent of built-in theme metrics.
for text in ("HEADER_STATUS_TOP = 10", "HEADER_STATUS_HEIGHT = 20",
             "HEADER_STATUS_EDGE_INSET = 22"):
    assert text in metrics, text
assert "status.batteryLeft = false;" in base
assert "status.clockCentered = false;" in base
assert "status.stripHeight = strip;" in base
assert "const int16_t strip = HEADER_STATUS_HEIGHT;" in base
assert "if (SETTINGS.clockShowInHeader &&" in base
assert "metrics.headerShowsClock &&" not in base
assert "status.clockCentered = metrics.headerClockCentered" not in base
# The band top moves to the shared status slot; bezel clearance stays in
# the board's headerOffsetY, so the viewable inset must not enter the shift.
shift_helper = base.split("int headerBandTopShift(", 1)[1].split("}  // namespace", 1)[0]
assert re.search(
    r"return\s+BaseTheme::HEADER_STATUS_TOP\s*-\s*"
    r"UITheme::getInstance\(\)\.getMetrics\(\)\.topPadding\s*;",
    shift_helper,
)
assert "getOrientedViewableTRBL" not in shift_helper
assert "static_cast<int16_t>(rect.height - offset)" in base
assert "headerBatterySide = 0" in rounded_metrics
assert "headerClockCentered = false" in lyra_metrics

# Only the status row moves: title and buttons must land where the theme's
# own strip height and topPadding put them, on every theme and board offset.
assert "const int16_t contentStrip = static_cast<int16_t>(metrics.batteryBarHeight);" in base
assert "opticalDrop - shift);" in base
assert ("props.titleOffsetY = static_cast<int16_t>(contentStrip / 2 + "
        "(BoardConfig::ACTIVE.headerOffsetY - shift) / 2);") in base
band_h, btn = 84, 48
for padding, content_strip in ((5, 20), (10, 20), (13, 24)):
    for board_offset in (0, 6, -1):
        shift = 10 - padding
        # Buttons: band top + 4 + actionOffsetY, before and after the shift.
        old_btn = padding + board_offset + 4 + content_strip + (band_h - content_strip - btn) // 2
        new_btn = (padding + board_offset + shift) + 4 + content_strip + (band_h - content_strip - btn) // 2 - shift
        assert new_btn == old_btn
        # Title center, doubled to stay in integers; C++ division truncates.
        old_center2 = 2 * (padding + board_offset) + (band_h - board_offset) + 2 * (
            content_strip // 2 + int(board_offset / 2))
        new_center2 = 2 * (padding + board_offset + shift) + (band_h - board_offset - shift) + 2 * (
            content_strip // 2 + int((board_offset - shift) / 2))
        assert abs(new_center2 - old_center2) <= 2, (padding, board_offset)

# A taller RoundedRaff Home band separates the clock from the book title
# without moving the bottom of its cover tile and menu.
assert ".homeTopPadding = 80," in rounded_metrics
assert ".homeCoverTileHeight = 325," in rounded_metrics
assert 80 + 325 == 64 + 341
assert "props.sidePadding = props.status.edgeInset;" in base
assert "props.titleText.align == fui::TextAlign::Left" in base

# Only a titled, left-aligned compact Home header with a visible clock gets
# optical balancing. The clock, band, cover and menu remain untouched.
assert "title && props.status.clockText && rect.height < UITheme::getInstance().getMetrics().headerHeight" in base
assert "ui.target.inkBounds(props.status.battery.text.font, props.status.clockText" in base
assert "ui.target.inkBounds(props.titleText.font, title, props.titleText)" in base
assert "constexpr int clockTitleGap = 12;" in base
assert "const int targetTop = std::min(clockTop + clockInk.height + clockTitleGap," in base
assert "band.bottom() - titleInk.height - 6);" in base
assert "props.titleOffsetY = static_cast<int16_t>(std::max(0, targetTop - titleTop));" in base

# The ink-based target gives the title 12px clearance from the clock
# whenever the compact band's cover-clearance bound does not intervene.
for band_y, band_h, clock_h, title_h, title_line_h, title_ink_y in (
        (16, 64, 12, 18, 24, 3), (10, 70, 12, 18, 24, 3), (18, 62, 11, 20, 27, 4)):
    clock_top = band_y + (20 - clock_h) // 2
    title_top = band_y + (band_h - title_line_h) // 2 + title_ink_y
    target = min(clock_top + clock_h + 12, band_y + band_h - title_h - 6)
    offset = max(0, target - title_top)
    assert title_top + offset == target
    assert target + title_h + 6 <= band_y + band_h
    if target == clock_top + clock_h + 12:
        assert target - (clock_top + clock_h) == 12

# RoundedRaff's Home screen must not suppress the shared status line when
# it draws a header without a book title.
header = rounded.split("void RoundedRaffTheme::drawHeader(", 1)[1].split(
    "void RoundedRaffTheme::drawRecentBookCover(", 1)[0]
# The delegation must be unconditional: an early return or a conditional
# before the call would allow a null Home title to suppress the status row.
delegate = "BaseTheme::drawHeader(renderer, rect, title, subtitle, backButton);"
assert header.count(delegate) == 1
before_delegate = re.sub(r"//[^\n]*|/\*.*?\*/", "", header.split(delegate, 1)[0], flags=re.S)
before_delegate = before_delegate.split("{", 1)[1]
assert not re.search(r"\b(if|switch|for|while|return|goto)\b", before_delegate)

# Verify the production geometry keeps the bottom edge fixed, including for
# short Home bands and when a board's headerOffsetY shifts the top edge.
band = base.split("freeink::ui::Rect BaseTheme::headerBand(", 1)[1].split(
    "void BaseTheme::drawHeader(", 1)[0]
assert "const int16_t shift = static_cast<int16_t>(headerBandTopShift());" in band
assert "const int16_t offset = static_cast<int16_t>(BoardConfig::ACTIVE.headerOffsetY + shift);" in band
assert re.search(r"rect\.y\s*\+\s*offset", band)
assert re.search(r"rect\.height\s*-\s*offset", band)
for band_y, band_height in ((5, 84), (10, 84), (13, 84), (0, 62),
                             (0, 76), (13, 24), (5, 51)):
    for board_offset in (0, 3, 6):
        shift = 10 - band_y
        offset = board_offset + shift
        # Match headerBand's Rect{rect.x + left, rect.y + offset,
        #                         rect.width - left - right, rect.height - offset}.
        rect_y, rect_height = band_y, band_height
        band_y_actual = rect_y + offset
        band_height_actual = rect_height - offset
        assert band_height_actual > 0
        assert band_y_actual + band_height_actual == rect_y + rect_height
print("Built-in theme header normalization: PASS")
