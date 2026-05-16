#include "PackageDetailActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>
#include <utility>

#include "MappedInputManager.h"
#include "PackageSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
std::string joinValues(const std::vector<std::string>& values) {
  std::string result;
  for (const auto& value : values) {
    if (!result.empty()) result += ", ";
    result += value;
  }
  return result;
}
}  // namespace

PackageDetailActivity::PackageDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             Marginalia::PackageManifest package)
    : Activity("PackageDetail", renderer, mappedInput), package_(std::move(package)) {}

void PackageDetailActivity::onEnter() {
  Activity::onEnter();
  selectedIndex_ = 0;
  rebuildRows();
  requestUpdate();
}

void PackageDetailActivity::onExit() {
  Activity::onExit();
  rows_.clear();
}

void PackageDetailActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int rowCount = static_cast<int>(rows_.size());
  if (rowCount <= 0) return;

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true);

  buttonNavigator_.onNextRelease([this, rowCount] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, rowCount);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, rowCount] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, rowCount);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, rowCount, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, rowCount, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, rowCount, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, rowCount, pageItems);
    requestUpdate();
  });
}

void PackageDetailActivity::rebuildRows() {
  rows_.clear();
  rows_.push_back(
      {StrId::STR_ENABLED, package_.enabled ? tr(STR_ENABLED) : tr(STR_DISABLED), RowAction::ToggleEnabled});
  rows_.push_back({StrId::STR_SETTINGS_TITLE,
                   package_.settings.empty() ? tr(STR_NO_EXTENSION_SETTINGS) : tr(STR_CONFIGURE),
                   package_.settings.empty() ? RowAction::None : RowAction::Settings});
  rows_.push_back(
      {StrId::STR_PERMISSIONS,
       package_.permissions.empty() ? std::string(tr(STR_NO_PERMISSIONS)) : joinValues(package_.permissions),
       RowAction::None});
  rows_.push_back({StrId::STR_COMPATIBILITY,
                   package_.compatible ? std::string(tr(STR_COMPATIBLE)) : package_.compatibilityError,
                   RowAction::None});
  rows_.push_back({StrId::STR_VERSION, package_.version, RowAction::None});
}

void PackageDetailActivity::handleSelection() {
  if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(rows_.size())) return;

  const auto& row = rows_[selectedIndex_];
  if (row.action == RowAction::ToggleEnabled) {
    if (package_.compatible && Marginalia::setPackageEnabled(package_.id, !package_.enabled)) {
      package_.enabled = !package_.enabled;
      rebuildRows();
      requestUpdate();
    }
  } else if (row.action == RowAction::Settings) {
    auto resultHandler = [this](const ActivityResult&) { requestUpdate(); };
    startActivityForResult(std::make_unique<PackageSettingsActivity>(renderer, mappedInput, package_), resultHandler);
  }
}

void PackageDetailActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, package_.name.c_str(),
                 package_.kind.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(rows_.size()), selectedIndex_,
      [this](int index) { return std::string(I18N[rows_[index].label]); }, nullptr, nullptr,
      [this](int index) { return rows_[index].value; }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
