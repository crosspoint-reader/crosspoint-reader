#!/bin/bash
set -e
cd "$(dirname "$0")"
# gtest is provided by Anaconda at /usr/local/anaconda3 (only dylib on this host).
# The system include /usr/include/gtest and Homebrew paths do not exist here,
# so the runner points at the conda prefix and embeds the rpath for the
# dylib. On hosts where gtest is installed under /usr/include/gtest or
# /opt/homebrew/include/gtest, swap the two -I flags and drop -L/-rpath.
g++ -std=c++20 -I ../../lib/Epub/Epub -I /usr/local/anaconda3/include \
  CjkLineBreakTest.cpp ../../lib/Epub/Epub/CjkLayout.cpp \
  -L /usr/local/anaconda3/lib -Wl,-rpath,/usr/local/anaconda3/lib \
  -lgtest -lgtest_main -pthread -o cjk_test
./cjk_test
