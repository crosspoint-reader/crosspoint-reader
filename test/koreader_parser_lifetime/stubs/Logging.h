#pragma once

#include <cstdio>

#define LOG_DBG(...) ((void)0)
#define LOG_ERR(tag, ...)               \
  do {                                  \
    std::fprintf(stderr, "[%s] ", tag); \
    std::fprintf(stderr, __VA_ARGS__);  \
    std::fputc('\n', stderr);           \
  } while (false)
