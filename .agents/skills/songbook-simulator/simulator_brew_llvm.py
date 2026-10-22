# Builds the desktop simulator with Homebrew LLVM (macOS host fallback).
# Older macOS Command Line Tools ship a libc++ without std::from_chars for
# float, which lib/Epub's CSS parser needs; Homebrew's libc++ has it.
# The native platform drops CC/CXX and detects gcc/g++ on PATH, so this puts
# gcc/g++ links to Homebrew clang first on the build PATH.
import os
import subprocess

Import("env")  # noqa: F821


def llvm_prefix():
    # Apple Silicon and Intel Homebrew use different prefixes.
    try:
        return subprocess.run(["brew", "--prefix", "llvm"], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "/opt/homebrew/opt/llvm"


LLVM = llvm_prefix()
SHIM_DIR = os.path.join(env.subst("$PROJECT_DIR"), "simulator-llvm.local")  # noqa: F821
os.makedirs(SHIM_DIR, exist_ok=True)
for name, target in (("gcc", "clang"), ("g++", "clang++"), ("cc", "clang"), ("c++", "clang++")):
    link = os.path.join(SHIM_DIR, name)
    if os.path.realpath(link) != os.path.realpath(os.path.join(LLVM, "bin", target)):
        if os.path.lexists(link):
            os.remove(link)
        os.symlink(os.path.join(LLVM, "bin", target), link)

env.PrependENVPath("PATH", SHIM_DIR)  # noqa: F821
env.PrependENVPath("PATH", os.path.join(LLVM, "bin"))  # noqa: F821
env.Append(  # noqa: F821
    LINKFLAGS=[
        f"-L{LLVM}/lib/c++",
        f"-Wl,-rpath,{LLVM}/lib/c++",
        f"-L{LLVM}/lib/unwind",
        "-lunwind",
    ]
)
