#pragma once

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
struct SerialCommand {
  enum class Type { Invalid, Ping, State, Screenshot, Click, Turn, Menu, Home, Open, Restart };
  enum class RestartTarget { Boot, Home, Reader };

  Type type = Type::Invalid;
  std::string_view name;   // as sent, for the reply
  const char* error = "";  // why an Invalid line was refused
  RemoteButton click = RemoteButton::None;
  bool forward = true;    // TURN
  bool chapter = false;   // TURN
  bool cold = false;      // OPEN
  std::string_view path;  // OPEN
  RestartTarget restart = RestartTarget::Boot;
};

namespace serial_command_detail {

constexpr std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return s;
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

  return invalid(cmd, "unknown command");
}
