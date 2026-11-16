"""Check that the dictionary setting stores a long folder name whole.

Compiles the production buildDictionarySetting() with the real field size and
copy helper against small host stubs.

Run: python3 test/test_dictionary_setting_name.py (requires a C++20 compiler).
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
settings_list = (ROOT / "src/SettingsList.h").read_text()
settings_header = (ROOT / "src/CrossPointSettings.h").read_text()
settings_source = (ROOT / "src/CrossPointSettings.cpp").read_text()
registry = (ROOT / "src/util/DictionaryRegistry.h").read_text()


def block(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def line(source, text):
    return next(candidate for candidate in source.splitlines() if text in candidate)


build_setting = block(settings_list, "inline SettingInfo buildDictionarySetting(")
copy_to_field = block(settings_source, "void copyToField(")
name_field = line(settings_header, "char dictionaryName[")
max_name_bytes = line(registry, "MAX_NAME_BYTES =")
setting_key = line(settings_list, "DICTIONARY_SETTING_KEY[] =")

program = f"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

struct DictionaryEntry {{
  std::string name;
  std::string stem;
}};
namespace DictionaryRegistry {{
{max_name_bytes}
}}
enum class StrId {{ STR_DICTIONARY, STR_NONE_OPT, STR_CAT_READER }};
enum class SettingType {{ ENUM }};
struct SettingInfo {{
  StrId nameId;
  const char* key;
  SettingType type;
  std::vector<std::string> enumStringValues;
  StrId category;
  std::function<uint8_t()> valueGetter;
  std::function<void(uint8_t)> valueSetter;
}};
struct {{
  const char* get(StrId) const {{ return "None"; }}
}} I18N;
struct CrossPointSettings {{
{name_field}
}};
CrossPointSettings SETTINGS;

{copy_to_field}
{setting_key}
{build_setting}

int main() {{
  // Two folders that differ only after byte 31, the old field's last byte.
  const std::string shared(31, 'a');
  const std::string first = shared + "-one";
  const std::string second = shared + "-two";
  const std::string longest(DictionaryRegistry::MAX_NAME_BYTES - 1, 'b');
  const SettingInfo setting = buildDictionarySetting({{{{first, "x"}}, {{second, "x"}}, {{longest, "x"}}}});
  assert(setting.enumStringValues.size() == 4);

  for (uint8_t option = 1; option <= 3; ++option) {{
    setting.valueSetter(option);
    assert(SETTINGS.dictionaryName == setting.enumStringValues[option] && "the folder name must be stored whole");
    assert(setting.valueGetter() == option);

    // Saved and loaded again, as CrossPointSettings::fromJson() does.
    const std::string saved = SETTINGS.dictionaryName;
    CrossPointSettings loaded;
    copyToField(loaded.dictionaryName, saved.c_str(), sizeof(loaded.dictionaryName));
    SETTINGS = loaded;
    assert(SETTINGS.dictionaryName == setting.enumStringValues[option]);
    assert(setting.valueGetter() == option && "the selection must survive a save and load");
  }}

  // A stored name that matches no folder shows as None.
  copyToField(SETTINGS.dictionaryName, shared.c_str(), sizeof(SETTINGS.dictionaryName));
  assert(setting.valueGetter() == 0);
  setting.valueSetter(0);
  assert(SETTINGS.dictionaryName[0] == '\\0');
  return 0;
}}
"""

with tempfile.TemporaryDirectory(prefix="dictionary-setting-") as directory:
    source = Path(directory) / "test.cpp"
    executable = Path(directory) / "test"
    source.write_text(program)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", str(source), "-o",
                    str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("Dictionary setting name regression checks passed")
