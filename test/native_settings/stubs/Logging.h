#pragma once
// Stub for native unit tests — swallows all log output
#include <cstdio>
#define LOG_DBG(tag, fmt, ...) ((void)0)
#define LOG_INF(tag, fmt, ...) ((void)0)
#define LOG_ERR(tag, fmt, ...) ((void)0)
