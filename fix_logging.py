import re

file_path = "src/activities/reader/EpubReaderActivity.cpp"

with open(file_path, "r") as f:
    content = f.read()

# Automatically remove all deprecated Serial.printf debug lines
content = re.sub(r'Serial\.printf\([\s\S]*?\);', '', content)

with open(file_path, "w") as f:
    f.write(content)

print("Logging statements removed. Ready for a clean PR.")
