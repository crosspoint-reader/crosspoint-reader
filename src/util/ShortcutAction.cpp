#include "ShortcutAction.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include "CrossPointSettings.h"
#include "ScreenshotUtil.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

ShortcutAction shortcutActionFromRawValue(const uint8_t value) {
  switch (value) {
    case shortcutActionRawValue(ShortcutAction::Sleep):
      return ShortcutAction::Sleep;
    case shortcutActionRawValue(ShortcutAction::PageTurn):
      return ShortcutAction::PageTurn;
    case shortcutActionRawValue(ShortcutAction::RefreshScreen):
      return ShortcutAction::RefreshScreen;
    case shortcutActionRawValue(ShortcutAction::Footnotes):
      return ShortcutAction::Footnotes;
    case shortcutActionRawValue(ShortcutAction::ToggleBookmark):
      return ShortcutAction::ToggleBookmark;
    case shortcutActionRawValue(ShortcutAction::SyncProgress):
      return ShortcutAction::SyncProgress;
    case shortcutActionRawValue(ShortcutAction::Screenshot):
      return ShortcutAction::Screenshot;
    case shortcutActionRawValue(ShortcutAction::FileBrowser):
      return ShortcutAction::FileBrowser;
    case shortcutActionRawValue(ShortcutAction::LookUpWord):
      return ShortcutAction::LookUpWord;
    case shortcutActionRawValue(ShortcutAction::FileTransfer):
      return ShortcutAction::FileTransfer;
    case shortcutActionRawValue(ShortcutAction::ToggleTiltPageTurn):
      return ShortcutAction::ToggleTiltPageTurn;
    case shortcutActionRawValue(ShortcutAction::Select):
      return ShortcutAction::Select;
    case shortcutActionRawValue(ShortcutAction::None):
    default:
      return ShortcutAction::None;
  }
}

bool isShortcutAvailableOutsideReader(const ShortcutAction action) {
  switch (action) {
    case ShortcutAction::Sleep:
    case ShortcutAction::RefreshScreen:
    case ShortcutAction::Screenshot:
    case ShortcutAction::FileBrowser:
    case ShortcutAction::FileTransfer:
    case ShortcutAction::ToggleTiltPageTurn:
      return true;
    case ShortcutAction::None:
    case ShortcutAction::PageTurn:
    case ShortcutAction::Footnotes:
    case ShortcutAction::ToggleBookmark:
    case ShortcutAction::SyncProgress:
    case ShortcutAction::LookUpWord:
    case ShortcutAction::Select:
      return false;
  }
  return false;
}

bool runGlobalShortcut(const ShortcutAction action, GfxRenderer& renderer) {
  switch (action) {
    case ShortcutAction::Sleep:
      enterDeepSleep(false);
      return true;
    case ShortcutAction::RefreshScreen:
      // The active activity gets first refusal so it can redraw its own content; only if it
      // declines do we push the existing framebuffer out again.
      if (!activityManager.handleForcedRefresh()) {
        RenderLock lock;
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      return true;
    case ShortcutAction::Screenshot:
      ScreenshotUtil::takeScreenshot(renderer);
      return true;
    case ShortcutAction::FileBrowser:
      activityManager.goToFileBrowser();
      return true;
    case ShortcutAction::FileTransfer:
      activityManager.goToFileTransfer();
      return true;
    case ShortcutAction::ToggleTiltPageTurn:
      SETTINGS.tiltPageTurn = SETTINGS.tiltPageTurn == CrossPointSettings::TILT_OFF ? CrossPointSettings::TILT_ON
                                                                                    : CrossPointSettings::TILT_OFF;
      SETTINGS.saveToFile();
      activityManager.requestUpdate();
      return true;
    case ShortcutAction::None:
    case ShortcutAction::PageTurn:
    case ShortcutAction::Footnotes:
    case ShortcutAction::ToggleBookmark:
    case ShortcutAction::SyncProgress:
    case ShortcutAction::LookUpWord:
    case ShortcutAction::Select:
      return false;
  }
  return false;
}
