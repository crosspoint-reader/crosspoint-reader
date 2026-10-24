#pragma once

#include <cstdint>
#include <string_view>

#include "RemoteButtonInput.h"

// One line of the USB serial control console, after its "CMD:" prefix: a command
// name in capitals, then its argument, if any. Pure, so the grammar is
// host-tested; SerialConsole carries the commands out.
//
//   PING                          uptime, to check the link
//   STATE                         screen in front, open book and heap
//   SCREENSHOT                    raw framebuffer (scripts/debugging_monitor.py)
//   CLICK CONFIRM|BACK|NEXT|PREV  one click, on any screen
//   TURN NEXT|PREV [CHAPTER]      a page or chapter turn in the reader
//   MENU                          the reader menu
//   HOME                          back to Home
//   OPEN [COLD] /path             open a book; COLD first drops its laid-out chapters
//   RESTART [HOME|READER]         reboot normally, or silently into Home or the open book
//   LS /path                      list a folder
//   GET [offset] /path            send a file, from offset (SerialTransfer.h)
//   PUT [REPLACE] size crc /path  receive a file: size in bytes, crc as 8 hex digits
//   MKDIR /path                   make a folder and its parents
//   RM /path                      remove a file, an empty folder or a font family
struct SerialCommand {
  enum class Type {
    Invalid,
    Ping,
    State,
    Screenshot,
    Click,
    Turn,
    Menu,
    Home,
    Open,
    Restart,
    List,
    Get,
    Put,
    MakeDir,
    Remove
  };
  enum class RestartTarget { Boot, Home, Reader };

  Type type = Type::Invalid;
  std::string_view name;   // as sent, for the reply
  const char* error = "";  // why an Invalid line was refused
  RemoteButton click = RemoteButton::None;
  bool forward = true;    // TURN
  bool chapter = false;   // TURN
  bool cold = false;      // OPEN
  std::string_view path;  // OPEN and the file commands
  RestartTarget restart = RestartTarget::Boot;
  bool replace = false;  // PUT
  uint32_t size = 0;     // PUT
  uint32_t crc = 0;      // PUT
  uint32_t offset = 0;   // GET
};

namespace serial_command_detail {

constexpr std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return s;
}

// Splits off the first space-separated word of s.
constexpr std::string_view takeWord(std::string_view& s) {
  const size_t space = s.find(' ');
  const std::string_view word = s.substr(0, space);
  s = space == std::string_view::npos ? std::string_view{} : trim(s.substr(space + 1));
  return word;
}

constexpr bool parseNumber(std::string_view text, uint32_t base, uint32_t& value) {
  if (text.empty() || text.size() > (base == 16 ? 8 : 10)) return false;
  uint64_t result = 0;
  for (const char c : text) {
    uint32_t digit;
    if (c >= '0' && c <= '9')
      digit = static_cast<uint32_t>(c - '0');
    else if (base == 16 && c >= 'a' && c <= 'f')
      digit = static_cast<uint32_t>(c - 'a' + 10);
    else if (base == 16 && c >= 'A' && c <= 'F')
      digit = static_cast<uint32_t>(c - 'A' + 10);
    else
      return false;
    result = result * base + digit;
  }
  if (result > UINT32_MAX) return false;
  value = static_cast<uint32_t>(result);
  return true;
}

constexpr SerialCommand invalid(SerialCommand cmd, const char* error) {
  cmd.type = SerialCommand::Type::Invalid;
  cmd.error = error;
  return cmd;
}

}  // namespace serial_command_detail

constexpr SerialCommand parseSerialCommand(std::string_view line) {
  using namespace serial_command_detail;
  using Type = SerialCommand::Type;

  line = trim(line);
  const size_t space = line.find(' ');
  SerialCommand cmd;
  cmd.name = line.substr(0, space);
  const std::string_view arg = space == std::string_view::npos ? std::string_view{} : trim(line.substr(space + 1));

  constexpr struct {
    std::string_view name;
    Type type;
  } bare[] = {{"PING", Type::Ping},
              {"STATE", Type::State},
              {"SCREENSHOT", Type::Screenshot},
              {"MENU", Type::Menu},
              {"HOME", Type::Home}};
  for (const auto& entry : bare) {
    if (cmd.name != entry.name) continue;
    if (!arg.empty()) return invalid(cmd, "takes no argument");
    cmd.type = entry.type;
    return cmd;
  }

  if (cmd.name == "CLICK") {
    if (arg == "CONFIRM")
      cmd.click = RemoteButton::Confirm;
    else if (arg == "BACK")
      cmd.click = RemoteButton::Back;
    else if (arg == "NEXT")
      cmd.click = RemoteButton::Next;
    else if (arg == "PREV")
      cmd.click = RemoteButton::Previous;
    else
      return invalid(cmd, "expects CONFIRM, BACK, NEXT or PREV");
    cmd.type = Type::Click;
    return cmd;
  }

  if (cmd.name == "TURN") {
    std::string_view direction = arg;
    constexpr std::string_view chapterSuffix = " CHAPTER";
    if (direction.size() > chapterSuffix.size() &&
        direction.substr(direction.size() - chapterSuffix.size()) == chapterSuffix) {
      cmd.chapter = true;
      direction = trim(direction.substr(0, direction.size() - chapterSuffix.size()));
    }
    if (direction == "NEXT")
      cmd.forward = true;
    else if (direction == "PREV")
      cmd.forward = false;
    else
      return invalid(cmd, "expects NEXT or PREV, optionally followed by CHAPTER");
    cmd.type = Type::Turn;
    return cmd;
  }

  if (cmd.name == "OPEN") {
    std::string_view path = arg;
    constexpr std::string_view coldPrefix = "COLD ";
    if (path.substr(0, coldPrefix.size()) == coldPrefix) {
      cmd.cold = true;
      path = trim(path.substr(coldPrefix.size()));
    }
    if (path.empty() || path.front() != '/') return invalid(cmd, "expects an absolute path");
    cmd.path = path;
    cmd.type = Type::Open;
    return cmd;
  }

  if (cmd.name == "RESTART") {
    if (arg.empty())
      cmd.restart = SerialCommand::RestartTarget::Boot;
    else if (arg == "HOME")
      cmd.restart = SerialCommand::RestartTarget::Home;
    else if (arg == "READER")
      cmd.restart = SerialCommand::RestartTarget::Reader;
    else
      return invalid(cmd, "expects nothing, HOME or READER");
    cmd.type = Type::Restart;
    return cmd;
  }

  constexpr struct {
    std::string_view name;
    Type type;
  } pathOnly[] = {{"LS", Type::List}, {"MKDIR", Type::MakeDir}, {"RM", Type::Remove}};
  for (const auto& entry : pathOnly) {
    if (cmd.name != entry.name) continue;
    if (arg.empty() || arg.front() != '/') return invalid(cmd, "expects an absolute path");
    cmd.path = arg;
    cmd.type = entry.type;
    return cmd;
  }

  if (cmd.name == "GET") {
    std::string_view rest = arg;
    if (!rest.empty() && rest.front() != '/') {
      if (!parseNumber(takeWord(rest), 10, cmd.offset)) return invalid(cmd, "expects [offset] /path");
    }
    if (rest.empty() || rest.front() != '/') return invalid(cmd, "expects [offset] /path");
    cmd.path = rest;
    cmd.type = Type::Get;
    return cmd;
  }

  if (cmd.name == "PUT") {
    std::string_view rest = arg;
    constexpr std::string_view replacePrefix = "REPLACE ";
    if (rest.substr(0, replacePrefix.size()) == replacePrefix) {
      cmd.replace = true;
      rest = trim(rest.substr(replacePrefix.size()));
    }
    if (!parseNumber(takeWord(rest), 10, cmd.size)) return invalid(cmd, "expects [REPLACE] size crc32 /path");
    const std::string_view crc = takeWord(rest);
    if (crc.size() != 8 || !parseNumber(crc, 16, cmd.crc)) return invalid(cmd, "expects [REPLACE] size crc32 /path");
    if (rest.empty() || rest.front() != '/') return invalid(cmd, "expects [REPLACE] size crc32 /path");
    cmd.path = rest;
    cmd.type = Type::Put;
    return cmd;
  }

  return invalid(cmd, "unknown command");
}
