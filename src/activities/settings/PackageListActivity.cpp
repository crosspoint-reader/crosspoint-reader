#include "PackageListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void PackageListActivity::onEnter() {
  Activity::onEnter();
  selectedIndex_ = 0;
  packageStore_.scan();
  requestUpdate();
}

void PackageListActivity::onExit() { Activity::onExit(); }

void PackageListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    finish();
    return;
  }

  const int packageCount = static_cast<int>(packageStore_.packages().size());
  if (packageCount <= 0) return;

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true);

  buttonNavigator_.onNextRelease([this, packageCount] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, packageCount);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, packageCount] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, packageCount);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, packageCount, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, packageCount, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, packageCount, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, packageCount, pageItems);
    requestUpdate();
  });
}

void PackageListActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& packages = packageStore_.packages();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PACKAGES),
                 Marginalia::PACKAGE_ROOT);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  if (packages.empty()) {
    const auto message = packageStore_.hadScanError() ? tr(STR_PACKAGE_SCAN_ERROR) : tr(STR_NO_PACKAGES);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, message);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(packages.size()), selectedIndex_,
        [&packages](int index) { return packages[index].name; },
        [&packages](int index) {
          const auto& package = packages[index];
          if (!package.summary.empty()) return package.summary;
          return package.id;
        },
        nullptr,
        [&packages](int index) {
          const auto& package = packages[index];
          return package.kind + " " + package.version;
        },
        true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
