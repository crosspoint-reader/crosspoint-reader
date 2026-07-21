#pragma once

#include <string>

#include "SavedClippingsModel.h"
#include "activities/Activity.h"
#include "clippings/ClippingStore.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class SavedClippingsActivity final : public Activity {
 public:
  explicit SavedClippingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SavedClippings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  int rowCount() const { return static_cast<int>(catalog_.entries.size()) + 1; }
  bool exportAvailable() const { return SavedClippingsModel::canExport(catalogLoadResult_, catalog_); }

  void reloadCatalog(bool clearNotice);
  void openSelectedBook();
  void handleClippingResult(const ActivityResult& result);
  void exportAll();

  std::string rowTitle(int index) const;
  std::string rowSubtitle(int index) const;
  std::string rowValue(int index) const;
  UIIcon rowIcon(int index) const;
  std::string statusText() const;

  ClippingStore::Catalog catalog_;
  ClippingStore::CatalogLoadResult catalogLoadResult_ = ClippingStore::CatalogLoadResult::DirectoryMissing;
  ClippingStore openedStore_;
  ButtonNavigator navigator_;
  int selectedIndex_ = 0;
  std::string notice_;
};
