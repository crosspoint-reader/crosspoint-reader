#pragma once

class MappedInputManager;
class TerminalWifi;

class TerminalInput {
 public:
  TerminalInput(MappedInputManager& input, TerminalWifi& wifi) : input(input), wifi(wifi) {}

  // Returns true when long Back requests a terminal polarity toggle. Power is
  // owned by TerminalActivity so its two-press exit prompt can update the UI.
  bool poll();

 private:
  static constexpr unsigned long LONG_PRESS_MS = 1000;

  MappedInputManager& input;
  TerminalWifi& wifi;

  void send(const char* sequence, unsigned int length) const;
};
