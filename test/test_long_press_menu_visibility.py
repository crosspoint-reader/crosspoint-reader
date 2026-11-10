"""Long-Press Menu is hidden, and does nothing, on touch boards.

Touch boards act on the text directly, and the X4 Pro Home key or the short
power click carries the other shortcuts, so the row does nothing there. Long-Press Menu is
built as a DynamicEnum (getter/setter, no valuePtr), so a valuePtr comparison
never matches it (#3589 broke #3516's guard that way). The rule has to identify
the row by something the row has.
"""

import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
settings_list = (ROOT / "src/SettingsList.h").read_text()
reader = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text()


def body_of(source, signature):
    start = source.index(signature)
    open_brace = source.index("{", start)
    depth = 0
    for i in range(open_brace, len(source)):
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        if depth == 0:
            return source[open_brace : i + 1]
    raise AssertionError(f"unbalanced body for {signature}")


builder = body_of(settings_list, "inline SettingInfo buildLongPressMenuSetting()")
assert "DynamicEnum(StrId::STR_LONG_PRESS_MENU" in builder, builder

ui_list = body_of(settings_list, "inline std::vector<SettingInfo> getSettingsList(")
hide = re.search(
    r"if \(BoardConfig::hasTouch\(\)\)\s*\{[^}]*?s\.nameId == StrId::STR_LONG_PRESS_MENU", ui_list
)
assert hide is not None, "getSettingsList() no longer hides Long-Press Menu on touch boards"
assert "valuePtr" not in hide.group(0), "a DynamicEnum row has no valuePtr: " + hide.group(0)

threshold = body_of(reader, "unsigned long EpubReaderActivity::confirmLongPressThreshold() const")
assert re.match(r"\{\s*(//[^\n]*\n\s*)*if \(BoardConfig::hasTouch\(\)\) return 0;", threshold), (
    "a stored Long-Press Menu value must not act where the row is hidden: " + threshold[:200]
)
