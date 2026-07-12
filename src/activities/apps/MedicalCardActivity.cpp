#include "MedicalCardActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

// ----------------------------------------------------------------
// Static helpers
// ----------------------------------------------------------------

const char* MedicalCardActivity::fieldLabel(int index) {
  switch (index) {
    case 0: return "Name";
    case 1: return "Blood Type";
    case 2: return "Allergies";
    case 3: return "Medications";
    case 4: return "Conditions";
    case 5: return "Emergency Contact";
    case 6: return "Emergency Phone";
    default: return "";
  }
}

char* MedicalCardActivity::fieldPtr(int index) {
  switch (index) {
    case 0: return info.name;
    case 1: return info.bloodType;
    case 2: return info.allergies;
    case 3: return info.medications;
    case 4: return info.conditions;
    case 5: return info.emergencyContact;
    case 6: return info.emergencyPhone;
    default: return nullptr;
  }
}

size_t MedicalCardActivity::fieldMaxLen(int index) {
  switch (index) {
    case 0: return sizeof(info.name) - 1;
    case 1: return sizeof(info.bloodType) - 1;
    case 2: return sizeof(info.allergies) - 1;
    case 3: return sizeof(info.medications) - 1;
    case 4: return sizeof(info.conditions) - 1;
    case 5: return sizeof(info.emergencyContact) - 1;
    case 6: return sizeof(info.emergencyPhone) - 1;
    default: return 0;
  }
}

// ----------------------------------------------------------------
// Storage
// ----------------------------------------------------------------

void MedicalCardActivity::loadFromSd() {
  memset(&info, 0, sizeof(info));
  auto file = Storage.open(SAVE_PATH);
  if (file) {
    file.read(reinterpret_cast<uint8_t*>(&info), sizeof(info));
    file.close();
  }
}

void MedicalCardActivity::saveToSd() {
  Storage.mkdir("/biscuit");
  auto file = Storage.open(SAVE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (file) {
    file.write(reinterpret_cast<const uint8_t*>(&info), sizeof(info));
    file.close();
  }
}

// ----------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------

void MedicalCardActivity::onEnter() {
  Activity::onEnter();
  loadFromSd();
  state = CARD_DISPLAY;
  fieldIndex = 0;
  requestUpdate();
}

void MedicalCardActivity::onExit() { Activity::onExit(); }

// ----------------------------------------------------------------
// Loop
// ----------------------------------------------------------------

void MedicalCardActivity::loop() {
  if (state == CARD_DISPLAY) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    // Long-press Confirm (checked on release) → QR view
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 800) {
      state = QR_VIEW;
      requestUpdate();
      return;
    }
    // Short press Confirm → edit select
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() < 800) {
      state = EDIT_SELECT;
      fieldIndex = 0;
      requestUpdate();
      return;
    }
    return;
  }

  if (state == EDIT_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = CARD_DISPLAY;
      requestUpdate();
      return;
    }

    buttonNavigator.onNext([this] {
      fieldIndex = ButtonNavigator::nextIndex(fieldIndex, FIELD_COUNT);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      fieldIndex = ButtonNavigator::previousIndex(fieldIndex, FIELD_COUNT);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const int idx = fieldIndex;
      const char* label = fieldLabel(idx);
      const char* current = fieldPtr(idx);
      const size_t maxLen = fieldMaxLen(idx);

      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, label,
                                                  current ? current : "", maxLen),
          [this, idx](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& text = std::get<KeyboardResult>(result.data).text;
              char* dst = fieldPtr(idx);
              if (dst) {
                const size_t max = fieldMaxLen(idx);
                strncpy(dst, text.c_str(), max);
                dst[max] = '\0';
                saveToSd();
              }
            }
          });
      return;
    }
    return;
  }

  if (state == QR_VIEW) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = CARD_DISPLAY;
      requestUpdate();
    }
    return;
  }
}

// ----------------------------------------------------------------
// Render
// ----------------------------------------------------------------

void MedicalCardActivity::render(RenderLock&& lock) {
  renderer.clearScreen();
  switch (state) {
    case CARD_DISPLAY:     renderDisplay();     break;
    case EDIT_SELECT: renderEditSelect();  break;
    case QR_VIEW:     renderQrView();      break;
  }
  renderer.displayBuffer();
}

void MedicalCardActivity::renderDisplay() const {
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics   = UITheme::getInstance().getMetrics();

  // Outer border with padding
  constexpr int BORDER_PAD = 6;
  renderer.drawRect(BORDER_PAD, BORDER_PAD, pageWidth - BORDER_PAD * 2, pageHeight - BORDER_PAD * 2, true);
  renderer.drawRect(BORDER_PAD + 2, BORDER_PAD + 2, pageWidth - (BORDER_PAD + 2) * 2,
                    pageHeight - (BORDER_PAD + 2) * 2, true);

  // --- Red cross symbol drawn with two filled rects ---
  constexpr int CROSS_SIZE  = 28;  // overall bounding box
  constexpr int CROSS_THICK = 10;  // arm thickness
  constexpr int CROSS_X     = 16;
  constexpr int CROSS_Y     = 16;
  // horizontal arm
  renderer.fillRect(CROSS_X, CROSS_Y + (CROSS_SIZE - CROSS_THICK) / 2, CROSS_SIZE, CROSS_THICK, true);
  // vertical arm
  renderer.fillRect(CROSS_X + (CROSS_SIZE - CROSS_THICK) / 2, CROSS_Y, CROSS_THICK, CROSS_SIZE, true);

  // Title
  const int titleX = CROSS_X + CROSS_SIZE + 8;
  const int titleY = CROSS_Y + (CROSS_SIZE - renderer.getTextHeight(UI_12_FONT_ID)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, titleY, "MEDICAL INFORMATION", true, EpdFontFamily::BOLD);

  // Separator under header
  const int sepY = CROSS_Y + CROSS_SIZE + 8;
  renderer.fillRect(BORDER_PAD + 4, sepY, pageWidth - (BORDER_PAD + 4) * 2, 2, true);

  // Content area
  const int contentX  = 16;
  const int lineH10   = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineHSm   = renderer.getLineHeight(SMALL_FONT_ID);
  int y               = sepY + 10;

  // Helper lambda — draw one labelled field
  auto drawField = [&](const char* label, const char* value) {
    renderer.drawText(SMALL_FONT_ID, contentX, y, label, true, EpdFontFamily::BOLD);
    y += lineHSm + 1;
    if (value && value[0] != '\0') {
      renderer.drawText(UI_10_FONT_ID, contentX + 4, y, value);
    } else {
      renderer.drawText(UI_10_FONT_ID, contentX + 4, y, "-");
    }
    y += lineH10 + 4;
  };

  // Name + Blood Type on same row to save vertical space
  renderer.drawText(SMALL_FONT_ID, contentX, y, "Name", true, EpdFontFamily::BOLD);
  const int btLabelX = pageWidth / 2;
  renderer.drawText(SMALL_FONT_ID, btLabelX, y, "Blood Type", true, EpdFontFamily::BOLD);
  y += lineHSm + 1;
  const char* nameVal = info.name[0] ? info.name : "-";
  const char* btVal   = info.bloodType[0] ? info.bloodType : "-";
  renderer.drawText(UI_10_FONT_ID, contentX + 4, y, nameVal);
  renderer.drawText(UI_10_FONT_ID, btLabelX + 4, y, btVal);
  y += lineH10 + 6;

  drawField("Allergies",   info.allergies);
  drawField("Medications", info.medications);
  drawField("Conditions",  info.conditions);

  // Separator before emergency section
  renderer.fillRect(BORDER_PAD + 4, y, pageWidth - (BORDER_PAD + 4) * 2, 1, true);
  y += 6;

  drawField("Emergency Contact", info.emergencyContact);
  drawField("Emergency Phone",   info.emergencyPhone);

  // Bottom note — small font, above button hints
  const int noteY = pageHeight - metrics.buttonHintsHeight - lineHSm - 8;
  renderer.drawCenteredText(SMALL_FONT_ID, noteY, "Card stays visible when device is off");

  const auto labels = mappedInput.mapLabels("Back", "Edit", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void MedicalCardActivity::renderEditSelect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Edit Medical Card");

  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH   = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight;

  // Build a flat parallel array of const char* pointers into info fields (stack-allocated)
  const char* const fieldValues[FIELD_COUNT] = {
      info.name, info.bloodType, info.allergies,
      info.medications, info.conditions, info.emergencyContact, info.emergencyPhone};

  GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, FIELD_COUNT, fieldIndex,
               [&fieldValues](int i) -> std::string {
                 char buf[64];
                 const char* val = fieldValues[i];
                 if (val && val[0] != '\0') {
                   snprintf(buf, sizeof(buf), "%s: %s", fieldLabel(i), val);
                 } else {
                   snprintf(buf, sizeof(buf), "%s: -", fieldLabel(i));
                 }
                 return buf;
               });

  const auto labels = mappedInput.mapLabels("Back", "Edit", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void MedicalCardActivity::renderQrView() const {
  const auto& metrics  = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Medical QR Code");

  // Build compact medical payload
  char qrBuf[512];
  snprintf(qrBuf, sizeof(qrBuf), "MED:NAME:%s;BT:%s;ALLG:%s;MED:%s;COND:%s;ICE:%s %s",
           info.name,
           info.bloodType,
           info.allergies,
           info.medications,
           info.conditions,
           info.emergencyContact,
           info.emergencyPhone);

  const int headerBottom = metrics.topPadding + metrics.headerHeight;
  const int qrAreaH      = pageHeight - headerBottom - metrics.buttonHintsHeight;
  // Square QR region centred in available area
  const int qrSize       = (qrAreaH < pageWidth ? qrAreaH : pageWidth) - 20;
  const int qrX          = (pageWidth - qrSize) / 2;
  const int qrY          = headerBottom + (qrAreaH - qrSize) / 2;

  QrUtils::drawQrCode(renderer, Rect{qrX, qrY, qrSize, qrSize}, qrBuf);

  const auto labels = mappedInput.mapLabels("Back", "Close", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
