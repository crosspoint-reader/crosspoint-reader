#pragma once

#include <stdio.h>

#define LOG_ERR(origin, format, ...) fprintf(stderr, "ERROR [" origin "]: " format "\n" __VA_OPT__(, ) __VA_ARGS__)
#define LOG_INF(origin, format, ...) fprintf(stderr, "INFO [" origin "]: " format "\n" __VA_OPT__(, ) __VA_ARGS__)
#define LOG_DBG(origin, format, ...) fprintf(stderr, "DEBUG [" origin "]: " format "\n" __VA_OPT__(, ) __VA_ARGS__)
