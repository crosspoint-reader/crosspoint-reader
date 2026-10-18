#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "activities/settings/SettingInfo.h"

// Reading and writing one settings row by value, for the two places that change settings from
// outside the settings screen: the web settings API (CrossPointWebServer) and the serial
// CMD:GET / CMD:SET (SerialTestApi). One set of rules for both, so a value the web page may set is
// exactly a value the serial command may set. Rows come from forEachSetting(), keyed by
// SettingInfo::key.
namespace SettingsByKey {

// The number a row holds: TOGGLE, ENUM and VALUE rows, and a slider row (ACTION) that persists a
// field. False for strings and for actions with nothing to read.
bool readNumber(const SettingInfo& s, int& out);

// How many options an ENUM row offers, and the label of one. The font-family rows count and name
// the families found on the card at run time, which their static rows leave empty.
uint8_t optionCount(const SettingInfo& s);
std::string optionLabel(const SettingInfo& s, uint8_t index);

enum class WriteResult : uint8_t { Applied, OutOfRange, NotWritable };

// TOGGLE: any non-zero is on. ENUM: 0 .. optionCount()-1. VALUE: its range. A slider row:
// 0 .. persistMax. Nothing is written when the value is out of range.
WriteResult writeNumber(const SettingInfo& s, int value);

// STRING rows, cut to the row's buffer.
WriteResult writeString(const SettingInfo& s, const std::string& value);

// After a batch of writes: settle the settings that depend on others, then save to the card.
void commit();

// Hands `visit` every row that has a key (`key` empty) or the row with that key, and returns how
// many it handed over. One walk for every caller that works by key, behind a plain function
// pointer, so a caller does not compile its own copy of forEachSetting()'s visitor.
using RowVisitor = void (*)(const SettingInfo& row, void* ctx);
int visitKeyed(std::string_view key, RowVisitor visit, void* ctx);

}  // namespace SettingsByKey
