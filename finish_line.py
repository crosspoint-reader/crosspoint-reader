import re

file_path = "src/activities/reader/EpubReaderActivity.cpp"
with open(file_path, "r") as f:
    content = f.read()

# 1. Put the variable declaration back right where it belongs
content = re.sub(
    r'(const uint16_t viewportHeight = renderer\.getScreenHeight\(\) - orientedMarginTop - orientedMarginBottom;\n)',
    r'\1    const bool useBold = (SETTINGS.forceBoldText == 1);\n',
    content
)

# 2. Add the exact line break the clang-format bot demanded
content = content.replace(
    "viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, useBold, popupFn)) {",
    "viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, useBold,\n                                      popupFn)) {"
)

with open(file_path, "w") as f:
    f.write(content)

print("Variable restored and line length fixed.")
