import re

file_path = "src/activities/reader/EpubReaderActivity.cpp"
with open(file_path, "r") as f:
    lines = f.readlines()

# 1. Strip invisible trailing spaces
lines = [line.rstrip() + '\n' for line in lines]
content = "".join(lines)

# 2. Collapse large gaps (3+ newlines) into a standard empty line (2 newlines)
content = re.sub(r'\n{3,}', '\n\n', content)

# 3. Remove empty lines right after an opening brace or right before a closing brace
content = re.sub(r'\{\n\n+', '{\n', content)
content = re.sub(r'\n\n+(\s*\})', r'\n\1', content)

# 4. Fix specific clang-format line wrapping
content = content.replace(
    "viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,\n                                      popupFn)) {",
    "viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, popupFn)) {"
)

# 5. Remove unused 'useBold' variable and orphaned comments in renderScreen()
content = re.sub(
    r'    bool useBold = \(SETTINGS\.forceBoldText == 1\);\n\n    // TURN ON GLOBAL BOLD FOR CACHE BUILDER\n\n',
    '',
    content
)
content = content.replace("    // TURN GLOBAL BOLD BACK OFF\n\n", "")
content = content.replace("  // IMMEDIATELY TURN OFF BOLD SO THE UI REMAINS NORMAL\n\n", "")
content = content.replace("    // TURN ON BOLD FOR GRAYSCALE PASSES\n\n", "")
content = content.replace("    // TURN BOLD OFF BEFORE FINAL FLUSH\n\n", "")

# 6. Fix the unused variable in renderContents() and restore the bold visual effect
content = content.replace(
    "if (SETTINGS.textAntiAliasing && !showHelpOverlay && !isNightMode) {\n    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft + 1, orientedMarginTop);\n  }",
    "if ((useBold || SETTINGS.textAntiAliasing) && !showHelpOverlay && !isNightMode) {\n    page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft + 1, orientedMarginTop);\n  }"
)

content = content.replace(
    "if (SETTINGS.textAntiAliasing && !showHelpOverlay && !isNightMode) {  // Don't anti-alias the help overlay",
    "if ((useBold || SETTINGS.textAntiAliasing) && !showHelpOverlay && !isNightMode) {  // Don't anti-alias the help overlay"
)

content = content.replace(
    "page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft + 1, orientedMarginTop);\n    renderer.copyGrayscaleLsbBuffers();",
    "if (useBold || SETTINGS.textAntiAliasing) {\n      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft + 1, orientedMarginTop);\n    }\n    renderer.copyGrayscaleLsbBuffers();"
)

content = content.replace(
    "page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft + 1, orientedMarginTop);\n    renderer.copyGrayscaleMsbBuffers();",
    "if (useBold || SETTINGS.textAntiAliasing) {\n      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft + 1, orientedMarginTop);\n    }\n    renderer.copyGrayscaleMsbBuffers();"
)

with open(file_path, "w") as f:
    f.write(content)

print("Formatting and CppCheck issues fixed!")
