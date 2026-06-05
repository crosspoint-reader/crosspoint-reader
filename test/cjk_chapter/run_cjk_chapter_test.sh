#!/bin/bash
set -e
cd "$(dirname "$0")"
g++ -std=c++20 -I ../../lib/Txt -I /usr/local/anaconda3/include \
  CjkChapterTest.cpp -L /usr/local/anaconda3/lib -Wl,-rpath,/usr/local/anaconda3/lib \
  -lgtest -lgtest_main -pthread -o chapter_test
./chapter_test
