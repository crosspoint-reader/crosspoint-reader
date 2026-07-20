#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "clippings/ClippingStore.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Metadata-only clipping browser. Text is loaded on demand in openDetail();
// list rendering never touches storage.
class ClippingListActivity final : public Activity {
 public:
  explicit ClippingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ClippingStore& store)
      : Activity("ClippingList", renderer, mappedInput), store_(store) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class View : uint8_t { List, Detail, DeletePrompt, DeleteConfirm };

  static constexpr unsigned long DELETE_HOLD_MS = 800;

  void cancelActivity();
  void handleBack();
  void handleConfirmRelease();
  void beginDelete();
  void deleteSelected();
  void openDetail();
  void finishWithJump();
  void wrapDetail();
  void moveList(int direction);
  void moveListPage(int direction);
  void moveDetailPage(int direction);

  Rect safeArea() const;
  Rect listArea() const;
  std::string headerTitle() const;
  std::string clippingTitle(int index) const;
  std::string clippingSubtitle(int index) const;
  void renderList(Rect screen) const;
  void renderDetail(Rect screen) const;
  void renderDelete(Rect screen) const;
  void drawHints() const;

  ClippingStore& store_;
  ButtonNavigator navigator_;
  View view_ = View::List;
  View deleteReturnView_ = View::List;
  int selectedIndex_ = 0;

  std::string detailText_;
  std::vector<std::string> detailLines_;
  int detailPage_ = 0;
  int detailPageCount_ = 1;
  int detailLinesPerPage_ = 1;

  bool confirmPressSeen_ = false;
  bool longPressHandled_ = false;
  bool ignoreConfirmRelease_ = false;
  bool deleteFailed_ = false;
};
