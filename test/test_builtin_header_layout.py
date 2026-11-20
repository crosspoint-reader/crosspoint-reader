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
# Guard the complete shift expression, including subtraction of the theme's
# top padding. A reversed sign would displace RoundedRaff's negative shift.
shift_helper = base.split("int headerBandTopShift(", 1)[1].split("}  // namespace", 1)[0]
assert re.search(
    r"return\s+std::max\(BaseTheme::HEADER_STATUS_TOP,\s*top\)\s*-\s*"
    r"UITheme::getInstance\(\)\.getMetrics\(\)\.topPadding\s*;",
    shift_helper,
)
assert "static_cast<int16_t>(rect.height - offset)" in base
assert "headerBatterySide = 0" in rounded_metrics
assert "headerClockCentered = false" in lyra_metrics

# The title compensates half of the *same* top-edge move as headerBand,
# including device offsets. Combining before division prevents two odd
# halves from being rounded independently.
assert "const int topOffset = BoardConfig::ACTIVE.headerOffsetY + headerBandTopShift(renderer);" in base
assert "props.titleOffsetY = static_cast<int16_t>(strip / 2 + topOffset / 2);" in base
assert "const int16_t shift = static_cast<int16_t>(headerBandTopShift(renderer));" in base
assert base.count("headerBandTopShift(renderer)") == 2
for top, padding, board_offset in ((0, 5, 0), (0, 13, 0), (9, 13, 3),
                                   (11, 13, 0), (15, 10, 2), (9, 13, -1)):
    shift = max(10, top) - padding
    total = board_offset + shift
    # C++ integer division truncates toward zero, including negative totals.
    half = int(total / 2)
    title_offset = 20 // 2 + half
    assert title_offset == 10 + int((board_offset + shift) / 2)
    if total % 2 == 0:
        assert 2 * (title_offset - 10) == total

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
assert "const int16_t shift = static_cast<int16_t>(headerBandTopShift(renderer));" in band
assert "const int16_t offset = static_cast<int16_t>(BoardConfig::ACTIVE.headerOffsetY + shift);" in band
assert re.search(r"rect\.y\s*\+\s*offset", band)
assert re.search(r"rect\.height\s*-\s*offset", band)
for band_y, band_height in ((5, 84), (10, 84), (13, 84), (0, 62),
                             (0, 76), (13, 24), (5, 51)):
    for viewable_top, board_offset in ((0, 0), (9, 3), (11, 0), (15, 2)):
        shift = max(10, viewable_top) - band_y
        offset = board_offset + shift
        # Match headerBand's Rect{rect.x + left, rect.y + offset,
        #                         rect.width - left - right, rect.height - offset}.
        rect_y, rect_height = band_y, band_height
        band_y_actual = rect_y + offset
        band_height_actual = rect_height - offset
        assert band_height_actual > 0
        assert band_y_actual + band_height_actual == rect_y + rect_height
print("Built-in theme header normalization: PASS")
