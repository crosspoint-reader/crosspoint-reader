#pragma once
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND

#include <atomic>
#include <cstdint>
#include <memory>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "network/LocalSendReceiver.h"

class LocalSendReceiveActivity final : public UiListActivity {
 public:
  static bool isAvailable();
  LocalSendReceiveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return listening && (!popup.isActive() || (receiver && receiver->hasActiveBatch())); }
  bool preventAutoSleep() override { return listening; }

 private:
  static constexpr int MAX_FOLDERS = 6;
  static constexpr int MAX_SESSION_FILES = 16;
  enum class FileAction : uint8_t { None, Open, Delete, ConfirmDelete };
  std::unique_ptr<LocalSendReceiver> receiver;
  OptionPopup popup;
  std::atomic<bool> checkingStorage{false};
  bool consentPopup = false;
  bool confirmHeld = false;
  bool listening = false;
  bool leaveRequested = false;
  FileAction pendingFileAction = FileAction::None;
  int pendingFileEntry = -1;
  char allowedFolders[MAX_FOLDERS][96]{};
  int folderCount = 0;
  char destination[96] = "/Books/LocalSend";
  char deviceId[33]{};
  char fingerprint[33]{};
  char ipAddress[24]{};
  char sessionNames[MAX_SESSION_FILES][129]{};
  int sessionCount = 0;
  uint32_t seenSequence = 0;
  freeink::ui::ListItem rows[MAX_SESSION_FILES + 1]{};

  static bool cancelled(void* context);
  bool loadDefinition();
  bool loadPreferences();
  void initializeReceiver();
  bool saveFingerprint();
  bool startReceiver();
  void showConsent();
  void showFileError(const char* message);
  void showFileDetails(int entry);
  void confirmDeleteFile(int entry);
  void handleFileAction(FileAction action, int entry);
  void recordCompleted();
  bool handleCustomInput() override;
  void onBackButton() override;
  int listCount() const override;
  const char* headerTitle() const override { return "Local Send"; }
  void activateIndex(int index) override;
  void buildScreen(UiScreen& screen) override;
  void renderOverlay() override;
};
#endif
