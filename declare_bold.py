PackageException: Can not create a symbolic link for `open-x4-sdk/libs/hardware/BatteryMonitor`, not a directorimport re

# 1. Declare the variable in the Header file
h_path = "lib/EpdFont/EpdFontFamily.h"
with open(h_path, "r") as f:
    h_content = f.read()

if "static bool globalForceBold;" not in h_content:
    # Insert the declaration right under 'public:'
    h_content = re.sub(r'(public:)', r'\1\n  static bool globalForceBold;', h_content)
    with open(h_path, "w") as f:
        f.write(h_content)

# 2. Initialize the variable in the CPP file
cpp_path = "lib/EpdFont/EpdFontFamily.cpp"
with open(cpp_path, "r") as f:
    cpp_content = f.read()

if "bool EpdFontFamily::globalForceBold" not in cpp_content:
    # Initialize it at the top of the file, right after the includes
    cpp_content = re.sub(r'(#include .*?\n)(?!#include)', r'\1\nbool EpdFontFamily::globalForceBold = false;\n', cpp_content, count=1)
    with open(cpp_path, "w") as f:
        f.write(cpp_content)

print("Variable formally declared. You've got this.")
