import re

# 1. EpdFontFamily.h - Inject declaration right before the class
h_path = "lib/EpdFont/EpdFontFamily.h"
with open(h_path, "r") as f: 
    text = f.read()
    
if "extern bool globalForceBold;" not in text:
    text = text.replace("class EpdFontFamily", "extern bool globalForceBold;\n\nclass EpdFontFamily")
    with open(h_path, "w") as f: 
        f.write(text)

# 2. EpdFontFamily.cpp - Inject definition after the first include
cpp_path = "lib/EpdFont/EpdFontFamily.cpp"
with open(cpp_path, "r") as f: 
    text = f.read()
    
if "bool globalForceBold = false;" not in text:
    text = re.sub(r'(#include.*?\n)', r'\1\nbool globalForceBold = false;\n\n', text, count=1)
    with open(cpp_path, "w") as f: 
        f.write(text)

# 3. EpubReaderActivity.cpp - Hook up the UI toggle
er_path = "src/activities/reader/EpubReaderActivity.cpp"
with open(er_path, "r") as f: 
    text = f.read()

# Clean up any leftover class scoping from previous scripts
text = text.replace("EpdFontFamily::globalForceBold", "globalForceBold")

# Wire it to the settings toggle
if "globalForceBold = useBold;" not in text:
    text = text.replace(
        "const bool useBold = (SETTINGS.forceBoldText == 1);",
        "const bool useBold = (SETTINGS.forceBoldText == 1);\n    globalForceBold = useBold;"
    )
with open(er_path, "w") as f: 
    f.write(text)

print("Brute-force injection complete.")
