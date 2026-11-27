#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Display formatting shared by the reading stats screens.
namespace statsformat {

// "2 h 30 min" or "45 min".
std::string duration(uint32_t seconds);

// Average length of the counted sessions, or "-" before the first one.
std::string averageSession(uint32_t sessionSeconds, uint32_t sessions);

// Pages per minute with one decimal, or "-" before a minute of reading.
std::string pagesPerMinute(uint32_t pages, uint32_t seconds);

// Today's reading time, as "12 / 20 min" against a goal, or "12 min" with none.
std::string today(uint32_t seconds, uint8_t goalMinutes);

std::string count(uint32_t value);

}  // namespace statsformat
