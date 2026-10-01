#pragma once

#include <FreeInkUI.h>
#include <ToneLut.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "util/ImageSettingsInput.h"

// Image viewer: full-screen BMP/PNG viewing with an Options modal for render
// settings, image info and file management. Tone state starts each session
// from the persisted SETTINGS.viewerRenderProfile; staged edits live in
// draftProfile until Apply.
//
// Modal model: ONE modal surface over the image. viewerPage selects which PAGE
// that surface shows (options / settings / info / delete confirm); page
// transitions replace the page contents inside the same panel rect — a child
// popup is never drawn on top of the parent menu. activeTone holds the ACTIVE
// session-global tone settings (consumed by the decoder); draftProfile holds
// staged values while the settings page is open (committed only by Apply).
enum class ToneParam : uint8_t { Brightness = 0, Gamma = 1, Contrast = 2, Quantizer = 3, Count = 4 };

enum class ViewerPage : uint8_t { Viewer, Options, ImageSettings, ImageInfo, DeleteConfirm };

// Actions of the Options page rows (order built at page build).
enum class ViewerAction : uint8_t { Settings = 0, Info, SleepCover, Delete };

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  static constexpr int MODAL_MAX_ROWS = 8;  // title header + largest page
  // Fullest page is Image Settings: each of the 4 stepper rows (one per
  // ToneParam) registers its row body plus the - and + controls, and the
  // Reset / Apply / sleep rows register one body hit each — 4*3 + 3 = 15
  // interactions; headers register none and there is no chrome guard.
  static constexpr int MODAL_MAX_INTERACTIONS = static_cast<int>(ToneParam::Count) * 3 + 3;
  static constexpr int MODAL_INTERACTION_CAPACITY = MODAL_MAX_INTERACTIONS;
  // Height of the pages' title header, exactly as fui::list draws it (raw
  // defaults: header text line + 2px gap + 1px underline).
  static int16_t modalHeaderHeight(const freeink::ui::DrawTarget& target);
  // Touch action space shared with the host-tested imageSettingsInput policy.
  static constexpr freeink::ui::ActionId ACTION_ROW = imageSettingsInput::kActionRow;
  static constexpr freeink::ui::ActionId ACTION_DECREMENT = imageSettingsInput::kActionDecrement;
  static constexpr freeink::ui::ActionId ACTION_INCREMENT = imageSettingsInput::kActionIncrement;

  void loadSiblingImages();
  void doSetSleepCover();
  bool canSetSleepCover() const;
  bool renderPng();
  void renderBmp(bool showPopup);
  // Canonical format-aware render entry for the CURRENT image: PNG goes
  // through the PNG converter/presentation path and never enters renderBmp();
  // BMP renders through the grayscale pipeline. showLoadingPopup gates the
  // BMP loading popup; the PNG path always shows its own decode progress.
  void renderCurrentImage(bool showLoadingPopup);
  void openOptionsMenu();
  void computeModalRect();
  void renderModal();
  void repaintModal();
  // Page transition inside the modal surface: closes the touch handshake
  // (OptionPopup's uiReady model) so touch routing can never dispatch against
  // the previous page's interaction table.
  void openModalPage(ViewerPage page);
  int buildPageItems();
  // Image Settings page: manual layout from SDK primitives (title header in
  // the list cadence, settingRow for the action rows, stepperRow for the four
  // editable ToneParam rows); every component registers its own interactions.
  void buildSettingsPage(freeink::ui::Frame<MODAL_INTERACTION_CAPACITY>& frame, const freeink::ui::Rect& body);
  int pageSelectableCount() const;
  void handleModalInput();
  void handleSettingsAxesInput();
  void activateRow();
  void modalBack();
  void adjustDraft(int delta);
  void applySettings();
  void resetDraft();
  void saveSleepProfile();
  // Transactional persist of the staged draft into one profile slot (viewer
  // or sleep): restores the slot and returns false when the settings write
  // fails.
  bool commitDraftProfile(ToneProfile& profile);
  void menuAction(ViewerAction action);
  void openInfoPage();
  void performDelete();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  bool isPng = false;

  // ONE modal cadence, resolved from the DeviceContext (modalRowHeight
  // policy): every modal page's visual row height AND touch minimum AND the
  // panel sizing use this single value — visual row height == touch minimum,
  // so a row's hit band can never bleed into its neighbor's.
  int16_t modalRowH = imageSettingsInput::MODAL_MIN_ROW_H;

  // Tone state (RAM-only, survives image navigation within the session):
  // activeTone is what the decoder consumes, draftProfile stages edits while
  // the settings page is open.
  ViewerPage viewerPage = ViewerPage::Viewer;
  int modalRow = 0;                                           // selected row of the current page
  ToneLut activeTone{};                                       // ACTIVE session-wide settings (consumed by the decoder)
  ToneProfile draftProfile{};                                 // staged values while the settings page is open
  std::vector<std::pair<std::string, std::string>> infoRows;  // filled by openInfoPage()

  // Options page row actions (dynamic: Set sleep cover appears only when
  // canSetSleepCover()).
  ViewerAction menuActions[MODAL_MAX_ROWS] = {};
  int menuActionCount = 0;

  // ONE modal surface: fixed panel rect for the whole modal session + page rows
  freeink::ui::Rect modalRect{};
  freeink::ui::ListItem modalItems[MODAL_MAX_ROWS] = {};
  char valueScratch[4][24] = {};             // settings page value strings
  std::string deleteHeadline;                // delete page filename scratch
  std::vector<std::string> deleteNameLines;  // delete page: wrapped full-width filename lines (filled at page entry)
  // Touch hit-testing (OptionPopup model): renderModal registers the rows on
  // the render pass and publishes the table when done; handleModalInput routes
  // touch snapshots against the published generation. modalUiReady stays open
  // across same-page repaints and closes on page transitions (openModalPage),
  // so a release is never routed against the previous page's table.
  freeink::ui::InteractionBuffer<MODAL_INTERACTION_CAPACITY> modalInteractions;
  std::atomic<bool> modalUiReady{false};
};
