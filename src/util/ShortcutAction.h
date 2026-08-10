#pragma once

#include <cstdint>

class GfxRenderer;

// One stable persisted action space for every configurable shortcut. Values 0..4 preserve the
// historical short-power meanings; append new actions without renumbering existing ones.
enum class ShortcutAction : uint8_t {
  None = 0,
  Sleep = 1,
  PageTurn = 2,
  RefreshScreen = 3,
  Footnotes = 4,
  ToggleBookmark = 5,
  SyncProgress = 6,
  Screenshot = 7,
  FileBrowser = 8,
  LookUpWord = 9,
  FileTransfer = 10,
  ToggleTiltPageTurn = 11,
  // Aliases the button to whatever Confirm currently means on screen. Unlike every other
  // action, this has no fixed effect to run — it is handled entirely at the input layer
  // (MappedInputManager::wasShortPowerSelectClick), so it is deliberately excluded from
  // isShortcutAvailableOutsideReader()/runGlobalShortcut()/runReaderShortcut().
  Select = 12,
};

constexpr uint8_t shortcutActionRawValue(const ShortcutAction action) { return static_cast<uint8_t>(action); }

ShortcutAction shortcutActionFromRawValue(uint8_t value);

// True when the action needs no open book, and so can run from anywhere. The rest are
// reader-only: they are ignored outside a reader rather than doing something surprising.
bool isShortcutAvailableOutsideReader(ShortcutAction action);

// Runs the actions that need no open book. Returns false for reader-only actions, PageTurn and
// None, all of which are the caller's to handle.
bool runGlobalShortcut(ShortcutAction action, GfxRenderer& renderer);

// Defined in main.cpp, where the sleep sequence lives.
void enterDeepSleep(bool fromTimeout);
