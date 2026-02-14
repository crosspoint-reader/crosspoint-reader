import re

# 1. Fix missing arguments in EpubReaderActivity.cpp
er_path = "src/activities/reader/EpubReaderActivity.cpp"
with open(er_path, "r") as f: 
    er = f.read()

er = er.replace(
    "SETTINGS.embeddedStyle)) {", 
    "SETTINGS.embeddedStyle, useBold)) {"
)
er = er.replace(
    "SETTINGS.embeddedStyle, popupFn)) {", 
    "SETTINGS.embeddedStyle, useBold, popupFn)) {"
)
with open(er_path, "w") as f: 
    f.write(er)


# 2. Fix clang-format line breaks in Section.cpp
sc_path = "lib/Epub/Epub/Section.cpp"
with open(sc_path, "r") as f: 
    sc = f.read()

sc = sc.replace(
    "sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(bool) +\n                                 sizeof(uint32_t);",
    "sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +\n                                 sizeof(bool) + sizeof(uint32_t);"
)
sc = sc.replace(
    "const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle, const bool useBold) {",
    "const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,\n                              const bool useBold) {"
)
with open(sc_path, "w") as f: 
    f.write(sc)


# 3. Fix clang-format line breaks in Section.h
sh_path = "lib/Epub/Epub/Section.h"
with open(sh_path, "r") as f: 
    sh = f.read()

sh = sh.replace(
    "uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle, bool useBold);",
    "uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,\n                       bool useBold);"
)
sh = sh.replace(
    "uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle, bool useBold,\n                         const std::function<void()>& popupFn = nullptr);",
    "uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,\n                         bool useBold, const std::function<void()>& popupFn = nullptr);"
)
with open(sh_path, "w") as f: 
    f.write(sh)

print("Arguments added and clang-format limits respected.")
