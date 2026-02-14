import os
import re

file_path = "src/activities/reader/EpubReaderActivity.cpp"

with open(file_path, "r") as f:
    content = f.read()

# 1. Remove references to the font library bold variable
content = re.sub(r'EpdFontFamily::globalForceBold\s*=\s*[a-zA-Z0-9_]+;', '', content)

# 2. Remove the extra 'useBold' argument from the Section parser calls
content = re.sub(r'SETTINGS\.embeddedStyle,\s*useBold\)\)', 'SETTINGS.embeddedStyle))', content)
content = re.sub(r'SETTINGS\.embeddedStyle,\s*useBold,', 'SETTINGS.embeddedStyle,', content)

with open(file_path, "w") as f:
    f.write(content)

print("Nuclear option applied! Library references removed.")
