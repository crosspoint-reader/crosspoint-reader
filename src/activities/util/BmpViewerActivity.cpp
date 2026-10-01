#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

#include "CrossPointSettings.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/ImageSettingsInput.h"

namespace fui = freeink::ui;

namespace {
constexpr char CUSTOM_SLEEP_ROOT_BMP[] = "/sleep.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_BMP[] = "/sleep-overlay.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_PNG[] = "/sleep-overlay.png";
constexpr size_t COPY_BUFFER_SIZE = 2048;

// Row cadence of the modal pages: the side padding of fui::list's raw
// defaults (sidePadding unset = 8px); the ROW height is the resolved
// modalRowH (see computeModalRect).
constexpr int16_t MODAL_SIDE_PAD = 8;

// Center the BMP/PNG image on the page (shared by the render paths).
void fitImageOnScreen(const int imageW, const int imageH, const int pageW, const int pageH, int* x, int* y) {
  if (imageW > pageW || imageH > pageH) {
    const float ratio = static_cast<float>(imageW) / static_cast<float>(imageH);
    const float screenRatio = static_cast<float>(pageW) / static_cast<float>(pageH);
    if (ratio > screenRatio) {
      *x = 0;
      *y = std::round((static_cast<float>(pageH) - static_cast<float>(pageW) / ratio) / 2);
    } else {
      *x = std::round((static_cast<float>(pageW) - static_cast<float>(pageH) * ratio) / 2);
      *y = 0;
    }
  } else {
    *x = (pageW - imageW) / 2;
    *y = (pageH - imageH) / 2;
  }
}

std::string baseNameOf(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  return (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
}
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput),
      filePath(std::move(path)),
      activeTone(toneLutFromProfile(SETTINGS.viewerRenderProfile)) {
  // Session start: the ACTIVE tone state comes from the persisted viewer
  // profile, exactly once per session. Image navigation re-runs onEnter() but
  // must NOT re-read settings here (it would clobber an in-session Apply);
  // after Apply the profile and activeTone already agree.
  buildToneLut(activeTone);
}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  const std::string fileName = baseNameOf(filePath);

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.') {
        std::string fname(name);
        if (FsHelpers::hasBmpExtension(fname) || FsHelpers::hasPngExtension(fname)) {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);

  const auto image = std::find(siblingImages.begin(), siblingImages.end(), fileName);
  if (image != siblingImages.end()) {
    currentImageIndex = static_cast<int>(image - siblingImages.begin());
  }
}

bool BmpViewerActivity::canSetSleepCover() const {
  return FsHelpers::hasBmpExtension(filePath) ||
         (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM &&
          FsHelpers::hasPngExtension(filePath));
}

bool BmpViewerActivity::renderPng() {
  ImageDimensions dimensions;
  if (!PngToFramebufferConverter::getDimensionsStatic(filePath, dimensions)) return false;
  if (dimensions.width <= 0 || dimensions.height <= 0) return false;

  const float scale = std::min(static_cast<float>(renderer.getScreenWidth()) / dimensions.width,
                               static_cast<float>(renderer.getScreenHeight()) / dimensions.height);
  const int width = std::min(renderer.getScreenWidth(), static_cast<int>(dimensions.width * std::min(scale, 1.0f)));
  const int height = std::min(renderer.getScreenHeight(), static_cast<int>(dimensions.height * std::min(scale, 1.0f)));
  RenderConfig config{(renderer.getScreenWidth() - width) / 2, (renderer.getScreenHeight() - height) / 2, width,
                      height};

  PngToFramebufferConverter converter;
  return converter.decodeToFramebuffer(filePath, renderer, config);
}

// ---------------------------------------------------------------------------
// Modal: ONE panel surface over the image; viewerPage picks the page that
// fills it. Page transitions replace the page rows inside the same rect.
// ---------------------------------------------------------------------------

void BmpViewerActivity::computeModalRect() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  // Standard popup width (the OptionPopup/fui dialog convention: 3/4 of the
  // screen, clamped by the theme's side margins), centered.
  const int width = std::min<int>(screenW * 3 / 4, screenW - metrics.optionPopupDialogSideMargin * 2);

  // Exact height of the LARGEST page — Image Settings — computed from the
  // same quantities buildSettingsPage() renders with (title header + the
  // page's rows at the resolved modal cadence) plus the panel's borders and
  // padding. The other pages are header + fewer rows (Options ≤ 4, Info 6,
  // Delete 5), so the settings page is the sizing invariant. Clamped to the
  // screen so a degenerate theme cannot push the panel off it.
  fui::GfxRendererTarget target = makeUiTarget(renderer);
  refreshSharedUiThemeTokens(target);
  // ONE cadence for every modal page (list rows, settings rows, touch
  // minimum, panel sizing): touch-capable targets raise the row height to the
  // device's touch minimum so ensureMinTouchRect() never expands a hit band
  // into the neighboring row; button-only targets keep the 36px density.
  const fui::DeviceContext device = target.deviceContext();
  modalRowH = static_cast<int16_t>(imageSettingsInput::modalRowHeight(device.hasTouch, device.minTouchSize));
  const int border = metrics.popupFrameThickness;
  const int height = std::min<int>(screenH, imageSettingsInput::modalBodyHeight(modalHeaderHeight(target), modalRowH,
                                                                                static_cast<int>(ToneParam::Count)) +
                                                border * 2 + 8);

  modalRect = fui::Rect{static_cast<int16_t>((screenW - width) / 2), static_cast<int16_t>((screenH - height) / 2),
                        static_cast<int16_t>(width), static_cast<int16_t>(height)};
}

int16_t BmpViewerActivity::modalHeaderHeight(const fui::DrawTarget& target) {
  // Raw list() header cadence: line height + 4 (the underline sits 2px below
  // the text). The header draws with the default TextStyle (font 0 =
  // FONT_SMALL), the same style buildSettingsPage() renders it with.
  return static_cast<int16_t>(target.lineHeight(fui::GfxRendererTarget::FONT_SMALL) + 4);
}

void BmpViewerActivity::renderModal() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int border = metrics.popupFrameThickness;

  // Opaque single surface: theme border + solid white body. Nothing of the
  // previous page (or the image) bleeds through.
  renderer.fillRect(modalRect.x, modalRect.y, modalRect.width, modalRect.height, true);
  renderer.fillRect(modalRect.x + border, modalRect.y + border, modalRect.width - border * 2,
                    modalRect.height - border * 2, false);

  // FreeInkUI frame setup, mirroring OptionPopup::render (raw primitives): the
  // frame registers each enabled row (and stepper control) on the interaction
  // buffer; the loop task routes touch snapshots against the published
  // generation (see handleModalInput). Physical buttons stay on the
  // mapped-input path, so the buffer is touch-only and never competes with
  // them for dispatch.
  fui::GfxRendererTarget target = makeUiTarget(renderer);
  refreshSharedUiThemeTokens(target);
  const fui::DeviceContext device = target.deviceContext();
  const fui::InputSnapshot noInput{};
  modalInteractions.beginPublishCycle();
  fui::Frame<MODAL_INTERACTION_CAPACITY> frame(target, device, noInput, modalInteractions);

  const fui::Rect body{static_cast<int16_t>(modalRect.x + border), static_cast<int16_t>(modalRect.y + border),
                       static_cast<int16_t>(modalRect.width - border * 2),
                       static_cast<int16_t>(modalRect.height - border * 2)};
  if (viewerPage == ViewerPage::ImageSettings) {
    // Stepper rows are SDK components, not list items — the settings page
    // lays out directly with the same cadence list() renders the other pages.
    buildSettingsPage(frame, body);
  } else {
    const int rows = buildPageItems();

    fui::ListProps props{};
    props.items = modalItems;
    props.count = static_cast<uint16_t>(rows);
    props.scrollIndicator = false;
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;
    // The resolved modal cadence as an explicit row height: the visual row
    // and list()'s device-minimum-based hit rect then agree, so
    // ensureMinTouchRect() never expands one row's band into the next.
    props.rowHeight = modalRowH;
    props.labelText.font = fui::GfxRendererTarget::FONT_BODY;
    props.valueText.font = fui::GfxRendererTarget::FONT_BODY;
    fui::list(frame, body, props);
  }
  modalInteractions.publish();
  // Handshake re-opens only after a complete table is published (OptionPopup
  // model): until the first publication of the current page, routing refuses.
  modalUiReady = true;

  // Button hints reflect the REAL page semantics: 1D pages use the standard
  // Back/Select/Up/Down labels; the 2-axis settings page shows Back | Apply
  // on every row whose Confirm applies the draft to the viewer (editable rows
  // and the Apply row), Back | Reset on the draft-reset row and Back | Save on
  // the dedicated sleep row — never a hint that Left/Right list rows.
  if (viewerPage == ViewerPage::ImageSettings) {
    const char* confirmLabel;
    if (modalRow == 0) {
      confirmLabel = tr(STR_RESET);
    } else if (modalRow == static_cast<int>(ToneParam::Count) + 2) {
      confirmLabel = tr(STR_SAVE);
    } else {
      confirmLabel = tr(STR_APPLY);  // editable value rows + the Apply row
    }
    const auto hint = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "-", "+");
    GUI.drawButtonHints(renderer, hint.btn1, hint.btn2, hint.btn3, hint.btn4);
  } else {
    const auto hint = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, hint.btn1, hint.btn2, hint.btn3, hint.btn4);
  }
}

void BmpViewerActivity::repaintModal() {
  renderModal();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

int BmpViewerActivity::buildPageItems() {
  int count = 0;
  menuActionCount = 0;
  int selectableIndex = 0;  // modalRow indexes ACTIONS, not visual rows
  // Adds a row. Selectable rows carry one stable action (ACTION_ROW) and their
  // actionValue IS the action index (the row Confirm/touch release activates);
  // headers and informational rows (selectable=false) are enabled=false —
  // list() registers no interaction for them (the SDK's non-interactive row),
  // and they never take focus — the focused visual row is the modalRow-th
  // SELECTABLE row, so what is visibly focused is always what Confirm will
  // execute.
  const auto add = [&](const char* label, const char* value, const bool header = false, const bool selectable = true) {
    auto& item = modalItems[count++];
    item = fui::ListItem{};
    item.label = label;
    item.value = value;
    item.isHeader = header;
    if (header || !selectable) {
      item.enabled = false;
      return;
    }
    item.actionValue = static_cast<int16_t>(selectableIndex);
    if (modalRow == selectableIndex) item.state = fui::StateFocused;
    ++selectableIndex;
  };

  switch (viewerPage) {
    case ViewerPage::Options:
      add(tr(STR_OPTIONS), nullptr, true);
      menuActionCount = 0;
      // Image Settings exist only for formats whose decode path consumes the
      // tone pipeline (BMP); PNG is offered Info/Delete/sleep-cover only.
      if (imageSettingsInput::imageSettingsAvailable(!isPng)) {
        add(tr(STR_IMAGE_SETTINGS), nullptr);
        menuActions[menuActionCount++] = ViewerAction::Settings;
      }
      add(tr(STR_INFO), nullptr);
      menuActions[menuActionCount++] = ViewerAction::Info;
      if (canSetSleepCover()) {
        add(tr(STR_SET_SLEEP_COVER), nullptr);
        menuActions[menuActionCount++] = ViewerAction::SleepCover;
      }
      add(tr(STR_DELETE), nullptr);
      menuActions[menuActionCount++] = ViewerAction::Delete;
      break;

    case ViewerPage::ImageInfo:
      add(tr(STR_IMAGE_INFO), nullptr, true);
      for (const auto& row : infoRows) {
        add(row.first.c_str(), row.second.c_str());
      }
      break;

    case ViewerPage::DeleteConfirm: {
      // Page order (documented + pinned by the host test): title header + Name
      // label + wrapped filename lines are informational (never selectable,
      // never focused); Cancel = action 0, Delete = action 1.
      add(tr(STR_DELETE), nullptr, true);
      add(tr(STR_NAME), nullptr, false, false);
      for (const auto& line : deleteNameLines) {
        add(line.c_str(), nullptr, false, false);
      }
      add(tr(STR_CANCEL), nullptr);
      add(tr(STR_DELETE), nullptr);
      break;
    }

    default:
      break;
  }
  return count;
}

void BmpViewerActivity::buildSettingsPage(fui::Frame<MODAL_INTERACTION_CAPACITY>& frame, const fui::Rect& body) {
  // Values come from the STAGED draft (activeTone is untouched until Apply).
  snprintf(valueScratch[0], sizeof(valueScratch[0]), "%u %%", draftProfile.brightnessPct);
  snprintf(valueScratch[1], sizeof(valueScratch[1]), "%d.%02d", draftProfile.gammaPct / 100,
           draftProfile.gammaPct % 100);
  snprintf(valueScratch[2], sizeof(valueScratch[2]), "%u %%", draftProfile.contrastPct);
  // Tri-state presentation, derived from the persisted value alone: -1 = no
  // override ("Default"), 0 = explicit Legacy, 1 = explicit Canonical.
  const char* quantizerText;
  switch (imageSettingsInput::quantizerLabel(draftProfile.quantizer)) {
    case imageSettingsInput::QuantizerLabel::Legacy:
      quantizerText = tr(STR_QUANTIZER_LEGACY);
      break;
    case imageSettingsInput::QuantizerLabel::Canonical:
      quantizerText = tr(STR_QUANTIZER_CANONICAL);
      break;
    default:
      quantizerText = tr(STR_QUANTIZER_DEFAULT);
      break;
  }
  snprintf(valueScratch[3], sizeof(valueScratch[3]), "%s", quantizerText);

  // Title header exactly as fui::list renders it (raw defaults: TextStyle{}
  // + a 1px underline 2px below, header row = line height + 4, shared with
  // computeModalRect's sizing via modalHeaderHeight()).
  int16_t cursorY = body.y;
  const fui::TextStyle headerStyle{};  // font 0 = FONT_SMALL, as list() renders headers
  const int16_t headerLh = frame.target().lineHeight(headerStyle.font);
  frame.target().text(fui::Rect{static_cast<int16_t>(body.x + MODAL_SIDE_PAD), cursorY,
                                static_cast<int16_t>(body.width - MODAL_SIDE_PAD * 2), headerLh},
                      tr(STR_IMAGE_SETTINGS), headerStyle);
  frame.target().fill(
      fui::Rect{static_cast<int16_t>(body.x + MODAL_SIDE_PAD), static_cast<int16_t>(cursorY + headerLh + 2),
                static_cast<int16_t>(body.width - MODAL_SIDE_PAD * 2), 1},
      fui::Paint::solid(fui::Color::Black));
  cursorY = static_cast<int16_t>(cursorY + modalHeaderHeight(frame.target()));

  // Shared row contract: a full-row hit with the row's actionValue (the value
  // Confirm and touch activation dispatch on); the focused row follows
  // modalRow; minTouchSize = the row height, so ensureMinTouchRect() never
  // expands a 36px hit band into the neighboring rows (its centered expansion
  // would hand boundary taps to the LATER row under newest-first routing).
  // settingRow registers the hit itself, so there is no manual hit-testing
  // anywhere on the page.
  fui::TextStyle valueStyle{};
  valueStyle.font = fui::GfxRendererTarget::FONT_BODY;
  const auto rowProps = [&](const char* label, const int actionValue) {
    fui::SettingRowProps row{};
    row.label = label;
    row.labelText.font = fui::GfxRendererTarget::FONT_BODY;
    row.valueText = valueStyle;
    row.action = ACTION_ROW;
    row.valueId = static_cast<int16_t>(actionValue);
    row.inputMask = fui::InputTouch;
    row.minTouchSize = modalRowH;
    if (modalRow == actionValue) row.state = fui::StateFocused;
    return row;
  };

  fui::SettingRowProps reset = rowProps(tr(STR_RESET_TO_DEFAULTS), 0);
  fui::settingRow(frame, fui::Rect{body.x, cursorY, body.width, modalRowH}, reset);
  cursorY = static_cast<int16_t>(cursorY + modalRowH);

  // The four editable ToneParam rows are SDK steppers: label + [- value +].
  // stepperRow registers the row body on the LABEL rect only (which ends
  // before the controls, so body and controls never overlap), then the
  // decrement/increment button hits — routePublished scans newest-first, so a
  // tap on +/- always resolves to the stepper control and can never also
  // reach the row body's Apply route. widestValue pins the value slot at the
  // row's widest rendering (the value-format maximum; for the quantizer the
  // pixel-widest localized label), so the controls hold still while stepping.
  const char* labels[] = {tr(STR_BRIGHTNESS), tr(STR_GAMMA), tr(STR_FILTER_CONTRAST), tr(STR_QUANTIZER)};
  const char* widest[] = {"110 %", "1.30", "130 %", quantizerText};
  // The quantizer's value slot must fit the widest localized label in pixels.
  for (const char* candidate : {tr(STR_QUANTIZER_DEFAULT), tr(STR_QUANTIZER_LEGACY), tr(STR_QUANTIZER_CANONICAL)}) {
    if (frame.target().measureText(fui::GfxRendererTarget::FONT_BODY, candidate, valueStyle).width >
        frame.target().measureText(fui::GfxRendererTarget::FONT_BODY, widest[3], valueStyle).width) {
      widest[3] = candidate;
    }
  }

  for (int param = 0; param < static_cast<int>(ToneParam::Count); ++param) {
    fui::StepperRowProps stepper{};
    stepper.row = rowProps(labels[param], param + 1);
    stepper.value = valueScratch[param];
    stepper.widestValue = widest[param];
    stepper.decrement = ACTION_DECREMENT;
    stepper.decrementValue = static_cast<int16_t>(param);
    stepper.increment = ACTION_INCREMENT;
    stepper.incrementValue = static_cast<int16_t>(param);
    fui::stepperRow(frame, fui::Rect{body.x, cursorY, body.width, modalRowH}, stepper);
    cursorY = static_cast<int16_t>(cursorY + modalRowH);
  }

  const int paramCount = static_cast<int>(ToneParam::Count);
  fui::SettingRowProps apply = rowProps(tr(STR_APPLY), paramCount + 1);
  fui::settingRow(frame, fui::Rect{body.x, cursorY, body.width, modalRowH}, apply);
  cursorY = static_cast<int16_t>(cursorY + modalRowH);
  fui::SettingRowProps sleep = rowProps(tr(STR_USE_FOR_SLEEP_RENDERING), paramCount + 2);
  fui::settingRow(frame, fui::Rect{body.x, cursorY, body.width, modalRowH}, sleep);
}

int BmpViewerActivity::pageSelectableCount() const {
  // Selectable rows of the current page (title headers are not selectable).
  switch (viewerPage) {
    case ViewerPage::Options: {
      // Must mirror buildPageItems: Image settings only for tone-capable
      // formats (BMP), then Info / Delete, plus Set sleep cover when allowed.
      int rows = 2;  // Info / Delete
      if (imageSettingsInput::imageSettingsAvailable(!isPng)) rows++;
      if (canSetSleepCover()) rows++;
      return rows;
    }
    case ViewerPage::ImageSettings:
      return static_cast<int>(ToneParam::Count) + 3;  // Reset + 4 value rows + Apply + Use for sleep rendering
    case ViewerPage::ImageInfo:
      return static_cast<int>(infoRows.size());
    case ViewerPage::DeleteConfirm:
      return 2;  // Cancel / Delete
    default:
      return 0;
  }
}

void BmpViewerActivity::openModalPage(const ViewerPage page) {
  // Closes the touch handshake until the new page's rows are published —
  // touch routing can never dispatch against the previous page's table.
  modalUiReady = false;
  viewerPage = page;
  modalRow = 0;
  repaintModal();  // publishes the new table and re-opens the handshake
}

void BmpViewerActivity::openOptionsMenu() {
  computeModalRect();
  openModalPage(ViewerPage::Options);
}

void BmpViewerActivity::menuAction(ViewerAction action) {
  // Page transitions inside the ONE modal surface — never a nested popup and
  // never an image re-render.
  switch (action) {
    case ViewerAction::Settings:
      // Stage: copy the active settings into the draft; nothing is committed
      // and the image is not re-rendered while stepping.
      draftProfile = toneProfileFromTone(activeTone);
      openModalPage(ViewerPage::ImageSettings);
      break;
    case ViewerAction::Info:
      openInfoPage();
      break;
    case ViewerAction::SleepCover:
      viewerPage = ViewerPage::Viewer;  // modal closes; the sleep-cover flow re-renders itself
      doSetSleepCover();
      break;
    case ViewerAction::Delete:
      deleteHeadline = baseNameOf(filePath);
      // Full-width wrapped filename lines (existing measured primitive: ≤2
      // lines, UTF-8-safe split for spaceless names, ellipsis on overflow).
      // Filled once per page entry, reused by repaints.
      deleteNameLines.clear();
      {
        const auto& metrics = UITheme::getInstance().getMetrics();
        const int sidePad = 8;  // list()'s raw sidePadding default
        const int rowW = modalRect.width - metrics.popupFrameThickness * 2 - sidePad * 2;
        deleteNameLines = renderer.wrappedText(uiScaleSpec().bodyFontId, deleteHeadline.c_str(), rowW, 2);
      }
      openModalPage(ViewerPage::DeleteConfirm);
      break;
  }
}

void BmpViewerActivity::openInfoPage() {
  infoRows.clear();
  infoRows.reserve(6);

  infoRows.emplace_back(tr(STR_NAME), baseNameOf(filePath));
  infoRows.emplace_back(tr(STR_PATH), filePath);

  // Dimensions, format, bit depth, file size through the existing decode APIs
  // only (no new parser). PNG: the converter exposes dimensions only.
  char line[80];
  if (isPng) {
    ImageDimensions dimensions;
    if (PngToFramebufferConverter::getDimensionsStatic(filePath, dimensions)) {
      snprintf(line, sizeof(line), "%d x %d", dimensions.width, dimensions.height);
      infoRows.emplace_back(tr(STR_SIZE), line);
    }
    infoRows.emplace_back(tr(STR_FORMAT), "PNG");
  } else {
    HalFile file;
    if (Storage.openFileForRead("BMP", filePath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        snprintf(line, sizeof(line), "%d x %d", bitmap.getWidth(), bitmap.getHeight());
        infoRows.emplace_back(tr(STR_SIZE), line);
        infoRows.emplace_back(tr(STR_FORMAT), "BMP");
        // Bitmap.is1Bit() classifies the pixel encoding; BW/gray are user
        // facing descriptions, so they come from the string table.
        snprintf(line, sizeof(line), "%ubpp %s", bitmap.getBpp(),
                 bitmap.is1Bit() ? tr(STR_BIT_DEPTH_BW) : tr(STR_BIT_DEPTH_GRAY));
        infoRows.emplace_back(tr(STR_BIT_DEPTH), line);
      } else {
        infoRows.emplace_back(tr(STR_FORMAT), tr(STR_INVALID_BMP_FILE));
      }
      snprintf(line, sizeof(line), "%u B", static_cast<unsigned>(file.size()));
      infoRows.emplace_back(tr(STR_FILE_SIZE), line);
    } else {
      infoRows.emplace_back(tr(STR_FORMAT), tr(STR_FILE_OPEN_FAILED));
    }
  }

  // Truncate oversized values (long paths) to the value column: the label
  // column and the text gap mirror fui::list's default row layout.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int border = metrics.popupFrameThickness;
  const int sidePad = 8;  // list()'s raw sidePadding default
  const int textGap = 10;
  const int labelColW = std::accumulate(infoRows.begin(), infoRows.end(), 0, [&](const int width, const auto& row) {
    return std::max(width, renderer.getTextWidth(UI_10_FONT_ID, row.first.c_str()));
  });
  const int availW = modalRect.width - border * 2 - sidePad * 2 - labelColW - textGap;
  const int valueFont = uiScaleSpec().bodyFontId;
  for (auto& row : infoRows) {
    if (renderer.getTextWidth(valueFont, row.second.c_str()) > availW) {
      row.second = renderer.truncatedText(valueFont, row.second.c_str(), availW);
    }
  }

  openModalPage(ViewerPage::ImageInfo);
}

void BmpViewerActivity::performDelete() {
  const std::string dirPath = FsHelpers::extractFolderPath(filePath);
  const int oldCount = static_cast<int>(siblingImages.size());
  const int deletedIndex = currentImageIndex;

  const bool removed = Storage.remove(filePath.c_str());
  if (!removed) {
    LOG_ERR("BMP", "Failed to delete %s", filePath.c_str());
  }
  // Viewer images have no generated cache artifacts in this firmware (there is
  // no removeImageCache() mechanism; BMP/PNG viewers decode from source), so
  // nothing else to clean up.

  // Delete contract: a failed physical delete aborts BEFORE any list/index
  // mutation — siblingImages/currentImageIndex/filePath stay untouched, the
  // viewer returns to the current image.
  const auto next = FsHelpers::imageIndexAfterRemove(removed, oldCount, deletedIndex);
  if (!next.has_value()) {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
    delay(1000);
    renderBmp(false);
    return;
  }
  if (deletedIndex < 0 || deletedIndex >= oldCount) return;  // inconsistent state: do not corrupt indices

  siblingImages.erase(siblingImages.begin() + deletedIndex);
  currentImageIndex = *next;
  if (*next < 0) {
    // Folder is empty: exit to the file browser at its folder.
    activityManager.goToFileBrowser(dirPath);
    return;
  }
  std::string navPath = dirPath;
  if (!navPath.empty() && navPath.back() != '/') navPath += "/";
  filePath = navPath + siblingImages[currentImageIndex];
  isPng = FsHelpers::hasPngExtension(filePath);
  onEnter();
}

void BmpViewerActivity::handleModalInput() {
  // The modal owns ALL input while viewerPage != Viewer: navigation moves
  // the page row, Confirm activates it, Back leaves the page. No viewer action
  // ever runs underneath.
  //
  // Touch routing first (OptionPopup model): renderModal registered each
  // enabled row (and stepper control) on the interaction buffer and published
  // that table. routePublished reads ONLY the published generation (never a
  // half-built one; modalUiReady gates the window until the current page's
  // first publication), and each routed event maps to exactly ONE semantic
  // action via the host-tested policy: a row body fires the same activateRow()
  // dispatch Confirm uses (modalRow = actionValue); a stepper control fires
  // the shared draft-adjustment path with the control's direction, moving the
  // focus onto the row it adjusts. No double-dispatch: the touch pass
  // returns, never falling through to the button path; taps outside the
  // registered interactions produce no event.
  const freeink::ui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
  if (snap.touchPressed || snap.touchReleased || snap.touchHeld) {
    if (modalUiReady) {
      const freeink::ui::ActionEvent event = modalInteractions.routePublished(snap);
      if (event) {
        switch (imageSettingsInput::settingsTouchActionFor(event.action)) {
          case imageSettingsInput::StepperTouch::ActivateRow:
            if (event.value >= 0 && event.value < pageSelectableCount()) {
              modalRow = event.value;
              activateRow();
            }
            return;
          case imageSettingsInput::StepperTouch::StepDown:
            if (event.value >= 0 && event.value < static_cast<int>(ToneParam::Count)) {
              modalRow = imageSettingsInput::editableRowForParam(event.value, static_cast<int>(ToneParam::Count));
              adjustDraft(-1);  // staged draft only; no image render until Apply
            }
            return;
          case imageSettingsInput::StepperTouch::StepUp:
            if (event.value >= 0 && event.value < static_cast<int>(ToneParam::Count)) {
              modalRow = imageSettingsInput::editableRowForParam(event.value, static_cast<int>(ToneParam::Count));
              adjustDraft(1);
            }
            return;
          default:
            return;  // routed but unknown: consumed, never falls through
        }
      }
    }
    if (snap.touchPressed && modalUiReady) {
      // Touch-down on a row moves the visible focus (OptionPopup highlight
      // pattern): the route() call above latched the hit as the active
      // interaction; read it back from the published table, no re-hit-testing.
      const int16_t idx = modalInteractions.activeIndex();
      if (idx >= 0) {
        const freeink::ui::Interaction& hit = modalInteractions.publishedData()[idx];
        if (hit.action == ACTION_ROW && hit.value >= 0 && hit.value < pageSelectableCount() && modalRow != hit.value) {
          modalRow = hit.value;
          repaintModal();
        }
      }
    }
    return;  // any touch pass while the modal is open is consumed by the modal
  }

  // Per-page input policy: Image Settings is a 2-AXIS page (Up/Down choose the
  // row, Left/Right modify the value) and therefore uses the explicit logical
  // buttons — NOT the merged NavNext/NavPrevious list navigation, whose
  // Down||Right / Up||Left merge would make one front Left/Right event match
  // both branches. The 1D pages (options / info / delete confirm) keep the
  // standard merged list navigation.
  if (viewerPage == ViewerPage::ImageSettings) {
    handleSettingsAxesInput();
    return;
  }

  const int rows = pageSelectableCount();
  if (rows <= 0) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    modalRow = (modalRow - 1 + rows) % rows;
    repaintModal();  // modal-only repaint; the image is never re-rendered
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext)) {
    modalRow = (modalRow + 1) % rows;
    repaintModal();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    modalBack();
    return;
  }
}

void BmpViewerActivity::handleSettingsAxesInput() {
  // Image Settings: explicit axes. Side Up/Down = vertical row selection;
  // front Left/Right = horizontal value stepping; Confirm/Back on release.
  // ALL events route through the ONE semantic mapping (imageSettingsInput::
  // actionFor) — dispatch is mutually exclusive (one event -> one branch ->
  // return), so a single event can never produce both a selection move and a
  // value change or a doubled activation.
  const auto dispatch = [&](const MappedInputManager::Button logical, const imageSettingsInput::Button axis) {
    return imageSettingsInput::actionFor(axis, mappedInput.wasPressed(logical), mappedInput.wasReleased(logical));
  };

  switch (dispatch(MappedInputManager::Button::Up, imageSettingsInput::Button::Up)) {
    case imageSettingsInput::Action::RowUp:
      modalRow = (modalRow - 1 + pageSelectableCount()) % pageSelectableCount();
      repaintModal();  // modal-only repaint; the image is never re-rendered
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Down, imageSettingsInput::Button::Down)) {
    case imageSettingsInput::Action::RowDown:
      modalRow = (modalRow + 1) % pageSelectableCount();
      repaintModal();
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Left, imageSettingsInput::Button::Left)) {
    case imageSettingsInput::Action::ValueDown:
      adjustDraft(-1);  // staged draft only; no image render until Apply
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Right, imageSettingsInput::Button::Right)) {
    case imageSettingsInput::Action::ValueUp:
      adjustDraft(1);
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Confirm, imageSettingsInput::Button::Confirm)) {
    case imageSettingsInput::Action::Activate:
      activateRow();
      return;
    default:
      break;
  }
  switch (dispatch(MappedInputManager::Button::Back, imageSettingsInput::Button::Back)) {
    case imageSettingsInput::Action::Back:
      modalBack();
      return;
    default:
      break;
  }
}

void BmpViewerActivity::activateRow() {
  switch (viewerPage) {
    case ViewerPage::Options:
      if (modalRow >= 0 && modalRow < menuActionCount) menuAction(menuActions[modalRow]);
      break;

    case ViewerPage::ImageSettings: {
      // Editable value rows AND the explicit Apply row share ONE viewer-Apply
      // implementation (Confirm applies without navigating to Apply); Reset
      // only restages the draft; the dedicated sleep row keeps its own action.
      // Routing is the pure, host-tested imageSettingsInput policy — mutually
      // exclusive by return.
      switch (imageSettingsInput::confirmActionForRow(modalRow, static_cast<int>(ToneParam::Count))) {
        case imageSettingsInput::RowAction::ApplyViewer:
          applySettings();
          break;
        case imageSettingsInput::RowAction::ResetDraft:
          resetDraft();
          break;
        case imageSettingsInput::RowAction::SaveSleepProfile:
          saveSleepProfile();
          break;
        default:
          break;
      }
      break;
    }

    case ViewerPage::ImageInfo:
      // Confirm returns to the root options page (same surface, no image render).
      openModalPage(ViewerPage::Options);
      break;

    case ViewerPage::DeleteConfirm:
      // Production-owned routing policy (host-tested): action 0 = Cancel,
      // action 1 = Delete, anything else = nothing (the destructive action is
      // never reachable through a visual-row fallback).
      switch (imageSettingsInput::deleteConfirmActionForRow(modalRow)) {
        case imageSettingsInput::DeleteAction::Cancel:
          // Cancel: back to the root options page.
          openModalPage(ViewerPage::Options);
          break;
        case imageSettingsInput::DeleteAction::Delete:
          // Delete: a failed remove aborts before any navigation state change.
          viewerPage = ViewerPage::Viewer;
          performDelete();
          break;
        default:
          break;
      }
      break;
  }
}

void BmpViewerActivity::modalBack() {
  switch (viewerPage) {
    case ViewerPage::Options:
      // Root page Back closes the modal; the image re-renders (grayscale
      // pipeline) to restore the panel content under it.
      viewerPage = ViewerPage::Viewer;
      renderBmp(false);
      break;
    case ViewerPage::ImageSettings:
      // Discard the staged draft (activeTone untouched) and return to the
      // root options page — modal-only repaint, no image re-render.
      openModalPage(ViewerPage::Options);
      break;
    case ViewerPage::ImageInfo:
    case ViewerPage::DeleteConfirm:
      openModalPage(ViewerPage::Options);
      break;
    default:
      break;
  }
}

void BmpViewerActivity::adjustDraft(int delta) {
  // Settings page: Left/Right adjust the STAGED draft only — no buildToneLut(),
  // no image render until Apply. The quantizer row steps directionally.
  // Explicit row->param mapping (imageSettingsInput policy): the Reset / Apply
  // / sleep action rows step nothing.
  if (viewerPage != ViewerPage::ImageSettings) return;
  const int param = imageSettingsInput::editableParamForRow(modalRow, static_cast<int>(ToneParam::Count));
  switch (param) {
    case static_cast<int>(ToneParam::Brightness):
      draftProfile.brightnessPct =
          imageSettingsInput::stepClamped(draftProfile.brightnessPct, delta, TONE_BRIGHTNESS_MIN, TONE_BRIGHTNESS_MAX);
      break;
    case static_cast<int>(ToneParam::Gamma):
      draftProfile.gammaPct =
          imageSettingsInput::stepClamped(draftProfile.gammaPct, delta, TONE_GAMMA_MIN, TONE_GAMMA_MAX);
      break;
    case static_cast<int>(ToneParam::Contrast):
      draftProfile.contrastPct =
          imageSettingsInput::stepClamped(draftProfile.contrastPct, delta, TONE_CONTRAST_MIN, TONE_CONTRAST_MAX);
      break;
    case static_cast<int>(ToneParam::Quantizer):
      // Directional cycle shared by the hardware Left/Right keys and the touch
      // stepper -/+ (imageSettingsInput policy): Default -> Canonical ->
      // Legacy -> Default forward, the reverse backward.
      draftProfile.quantizer = imageSettingsInput::quantizerStepped(draftProfile.quantizer, delta);
      break;
    default:
      return;  // action rows: Left/Right is a no-op
  }
  repaintModal();
}

bool BmpViewerActivity::commitDraftProfile(ToneProfile& profile) {
  // ONE transactional persist implementation for both profile saves. The
  // settings write is the commit boundary: the slot takes the normalized draft
  // candidate, and a failed save restores the previous value — the store never
  // ends up with a profile the disk does not confirm (no false success).
  ToneProfile candidate = draftProfile;
  normalizeToneProfile(candidate);
  const ToneProfile previous = profile;
  profile = candidate;
  if (!SETTINGS.saveToFile()) {
    profile = previous;
    return false;
  }
  return true;
}

void BmpViewerActivity::applySettings() {
  // The ONLY viewer-Apply implementation: commit the draft once, close the
  // ENTIRE modal UI, and render the current image exactly once — no return to
  // the root menu first. Reachable from every editable row and the Apply row.
  //
  // Persistence is the commit boundary (transactional): the staged draft goes
  // to SETTINGS.viewerRenderProfile and only a successful saveToFile() lets it
  // become the activeTone. A failed save restores the previous profile —
  // RAM never ends up "active but unsaved" — shows the failure indication,
  // stays on the settings page with the draft intact, changes no activeTone
  // and re-renders no image. sleepRenderProfile is never touched here (and
  // "Use for sleep rendering" never touches this profile).
  if (!commitDraftProfile(SETTINGS.viewerRenderProfile)) {
    LOG_ERR("BMP", "Failed to save viewer render profile");
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
    delay(1000);
    repaintModal();
    return;
  }
  activeTone = toneLutFromProfile(SETTINGS.viewerRenderProfile);
  buildToneLut(activeTone);
  viewerPage = ViewerPage::Viewer;
  renderBmp(false);
}

void BmpViewerActivity::resetDraft() {
  // "Reset to defaults": restage ONLY the draft (B100 G1.00 C100, quantizer =
  // no override) and stay on the settings page. Nothing is applied, persisted
  // or rendered yet — the user can inspect/modify the reset draft before
  // Apply; Back discards it without touching activeTone or either profile.
  draftProfile = ToneProfile{};
  repaintModal();
}

void BmpViewerActivity::saveSleepProfile() {
  // "Use for sleep rendering": persist the STAGED draft (not activeTone — the
  // two may differ) as the sleep render profile. Transactional (see
  // commitDraftProfile): a failed SD write restores the previous profile (no
  // unsaved-but-active state), shows the failure popup (never a false Done),
  // and stays on the settings page with the draft intact for a retry. No
  // image render, no activeTone change, no viewerRenderProfile change, no
  // sleep-cover file/mode touch.
  if (!commitDraftProfile(SETTINGS.sleepRenderProfile)) {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
    delay(1000);
    repaintModal();
    return;
  }
  GUI.drawPopup(renderer, tr(STR_DONE));
  delay(1000);
  openModalPage(ViewerPage::Options);
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  isPng = FsHelpers::hasPngExtension(filePath);

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  if (isPng) {
    Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
    renderer.clearScreen();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPTIONS), "", "");
    if (renderPng()) {
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
      GUI.drawButtonHints(renderer, labels.btn1, "", "", "");
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    return;
  }

  renderBmp(true);
}

void BmpViewerActivity::renderBmp(bool showPopup) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect;
  if (showPopup) {
    popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
  }

  HalFile file;
  // 1. Open the BMP file
  if (!Storage.openFileForRead("BMP", filePath, file)) {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  // Always attach the active tone: quantizer selection is active even when
  // the tone LUT itself is at identity (enabled=false).
  Bitmap bitmap(file, true,
                renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported() &&
                    display.getController() == HalDisplay::Controller::SSD1677,
                &activeTone);

  // 2. Parse headers to get dimensions
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    // Handle file parsing error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INVALID_BMP_FILE));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  int x, y;
  fitImageOnScreen(bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight, &x, &y);

  // 4. Prepare Rendering
  bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
  bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                  currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

  // Confirm opens the Options modal; its hint reads as an active viewer action.
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPTIONS), (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));

  if (showPopup) GUI.fillPopupProgress(renderer, popupRect, 50);

  renderer.clearScreen();
  if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  // Draw UI hints on the base layer
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (bitmap.hasGreyscale()) {
    const bool absolute = renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
    if (absolute && !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute)) return;
    if (!absolute) renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
    bool planesReady = true;
    for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      if (bitmap.rewindToData() != BmpReaderError::Ok) {
        LOG_ERR("BMP", "Failed to rewind bitmap for grayscale rendering");
        planesReady = false;
        break;
      }
      renderer.clearScreen(absolute ? 0xFF : 0x00);
      renderer.setRenderMode(mode);
      if (!renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
        planesReady = false;
        break;
      }
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      if (mode == GfxRenderer::GRAYSCALE_LSB) {
        renderer.copyGrayscaleLsbBuffers();
      } else {
        renderer.copyGrayscaleMsbBuffers();
      }
    }
    if (planesReady) renderer.displayGrayBuffer();

    // Rebuild the BW framebuffer for popups and subsequent differential updates.
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.clearScreen();
    if (bitmap.rewindToData() != BmpReaderError::Ok ||
        !renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
      LOG_ERR("BMP", "Failed to rewind bitmap to restore the BW framebuffer");
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
      planesReady = false;
    }
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.cleanupGrayscaleWithFrameBuffer();
    if (!planesReady) renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  const bool transparentMode = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM;
  if (!canSetSleepCover()) return;

  const char* destination =
      transparentMode ? (FsHelpers::hasPngExtension(filePath) ? TRANSPARENT_SLEEP_ROOT_PNG : TRANSPARENT_SLEEP_ROOT_BMP)
                      : CUSTOM_SLEEP_ROOT_BMP;
  bool success = filePath == destination;

  if (!success) {
    auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUFFER_SIZE);
    if (!buffer) {
      LOG_ERR("BMP", "OOM: sleep cover copy buffer");
    } else {
      // Copy beside the target and swap in only a complete image, so a failed
      // copy keeps the previous sleep cover instead of a truncated one.
      const std::string tmp = std::string(destination) + ".tmp";
      HalFile inFile, outFile;
      if (Storage.openFileForRead("BMP", filePath, inFile) && Storage.openFileForWrite("BMP", tmp, outFile)) {
        int bytesRead;
        success = true;
        while ((bytesRead = inFile.read(buffer.get(), COPY_BUFFER_SIZE)) > 0) {
          if (outFile.write(buffer.get(), static_cast<size_t>(bytesRead)) != static_cast<size_t>(bytesRead)) {
            success = false;
            break;
          }
        }
        if (bytesRead < 0) success = false;
        outFile.close();
        success = success && Storage.replaceFile(tmp.c_str(), destination);
        if (!success) Storage.remove(tmp.c_str());
      }
    }
  }

  if (success) {
    if (!transparentMode) SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }

  delay(1000);
  onEnter();
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  auto openSibling = [this](const int delta) {
    if (currentImageIndex < 0) {
      return false;
    }
    const int nextIndex = currentImageIndex + delta;
    if (siblingImages.size() <= 1 || nextIndex < 0 || nextIndex >= static_cast<int>(siblingImages.size())) {
      return false;
    }
    currentImageIndex = nextIndex;
    std::string dirPath = FsHelpers::extractFolderPath(filePath);
    if (dirPath.back() != '/') dirPath += "/";
    filePath = dirPath + siblingImages[currentImageIndex];
    isPng = FsHelpers::hasPngExtension(filePath);
    onEnter();
    return true;
  };

  // ONE modal owner: while viewerPage != Viewer the modal surface owns ALL
  // input and loop() always returns after its handler — no sibling navigation,
  // no viewer buttons, no image repaint underneath.
  if (viewerPage != ViewerPage::Viewer) {
    handleModalInput();
    return;
  }

  // Viewer mode — native Image Viewer controls plus the Options modal trigger:
  //   Up (side) / Left (front) -> previous image
  //   Down (side) / Right (front) -> next image
  //   Confirm (GPIO8) -> Options modal
  //   Back -> exit to the file browser
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    openSibling(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openOptionsMenu();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openSibling(1);
    return;
  }
}
