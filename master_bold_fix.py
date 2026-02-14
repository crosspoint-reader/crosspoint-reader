import re

# 1. EpdFontFamily.cpp - Use actual bold fonts natively
with open("lib/EpdFont/EpdFontFamily.cpp", "r") as f: ef = f.read()
if "|| globalForceBold" not in ef:
    ef = ef.replace(
        "const bool hasBold = (style & BOLD) != 0;",
        "const bool hasBold = ((style & BOLD) != 0) || globalForceBold;"
    )
    with open("lib/EpdFont/EpdFontFamily.cpp", "w") as f: f.write(ef)

# 2. Section.h - Add useBold cache parameter
with open("lib/Epub/Epub/Section.h", "r") as f: sh = f.read()
sh = re.sub(r'(bool hyphenationEnabled, bool embeddedStyle)(\);)', r'\1, bool useBold\2', sh)
sh = re.sub(r'(bool hyphenationEnabled, bool embeddedStyle)(,\n\s*const std::function)', r'\1, bool useBold\2', sh)
with open("lib/Epub/Epub/Section.h", "w") as f: f.write(sh)

# 3. Section.cpp - Rebuild cache when wider letters are used
with open("lib/Epub/Epub/Section.cpp", "r") as f: sc = f.read()
if "const bool useBold" not in sc:
    sc = sc.replace("SECTION_FILE_VERSION = 12;", "SECTION_FILE_VERSION = 13;")
    sc = sc.replace("sizeof(bool) + sizeof(bool) +\n                                 sizeof(uint32_t)", "sizeof(bool) + sizeof(bool) + sizeof(bool) +\n                                 sizeof(uint32_t)")
    sc = re.sub(r'(const bool embeddedStyle)(\) \{)', r'\1, const bool useBold\2', sc)
    sc = sc.replace("serialization::writePod(file, embeddedStyle);", "serialization::writePod(file, embeddedStyle);\n  serialization::writePod(file, useBold);")
    sc = sc.replace("bool fileEmbeddedStyle;", "bool fileEmbeddedStyle;\n    bool fileUseBold;")
    sc = sc.replace("serialization::readPod(file, fileEmbeddedStyle);", "serialization::readPod(file, fileEmbeddedStyle);\n    serialization::readPod(file, fileUseBold);")
    sc = sc.replace("embeddedStyle != fileEmbeddedStyle)", "embeddedStyle != fileEmbeddedStyle || useBold != fileUseBold)")
    sc = re.sub(r'(viewportHeight, hyphenationEnabled, embeddedStyle)(\);)', r'\1, useBold\2', sc)
    with open("lib/Epub/Epub/Section.cpp", "w") as f: f.write(sc)

# 4. EpubReaderActivity.cpp - Apply the exact clang-format request
with open("src/activities/reader/EpubReaderActivity.cpp", "r") as f: er = f.read()
er = re.sub(
    r'if \(\(useBold \|\| SETTINGS\.textAntiAliasing\) && !showHelpOverlay && !isNightMode\) \{  // Don\'t anti-alias the help overlay',
    r'if ((useBold || SETTINGS.textAntiAliasing) && !showHelpOverlay &&\n      !isNightMode) {  // Don\'t anti-alias the help overlay',
    er
)
with open("src/activities/reader/EpubReaderActivity.cpp", "w") as f: f.write(er)

print("True bold cache logic applied and clang-format fixed.")
