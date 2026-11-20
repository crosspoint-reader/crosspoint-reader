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
assert "std::max(BaseTheme::HEADER_STATUS_TOP, top)" in base
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
assert "BaseTheme::drawHeader(renderer, rect, title, subtitle, backButton);" in header
assert "if (title == nullptr)" not in header

# The header keeps its previous bottom edge across built-in theme geometry.
for top, height in ((5, 84), (10, 84), (13, 84), (0, 62), (0, 76)):
    shift = max(10, 0) - top
    assert height - shift > 20
print("Built-in theme header normalization: PASS")
