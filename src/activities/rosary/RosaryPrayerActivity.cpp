#include "RosaryPrayerActivity.h"

#include <GfxRenderer.h>

#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void RosaryPrayerActivity::taskTrampoline(void* param) {
  auto* self = static_cast<RosaryPrayerActivity*>(param);
  self->displayTaskLoop();
}

void RosaryPrayerActivity::onEnter() {
  Activity::onEnter();
  renderingMutex = xSemaphoreCreateMutex();
  currentStep = 0;
  updateRequired = true;

  xTaskCreate(&RosaryPrayerActivity::taskTrampoline, "RosaryPrayerTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void RosaryPrayerActivity::onExit() {
  Activity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void RosaryPrayerActivity::loop() {
  // Back button: go back to rosary menu
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onComplete();
    return;
  }

  // Volume Down / Right / Confirm: advance to next bead
  buttonNavigator.onNextRelease([this] {
    if (currentStep < RosaryData::TOTAL_STEPS - 1) {
      currentStep++;
      updateRequired = true;
    }
  });

  // Volume Up / Left: go to previous bead
  buttonNavigator.onPreviousRelease([this] {
    if (currentStep > 0) {
      currentStep--;
      updateRequired = true;
    }
  });
}

void RosaryPrayerActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

std::string RosaryPrayerActivity::getStepTitle() const {
  auto step = RosaryData::getStep(currentStep);
  if (step.type == RosaryData::BeadType::MysteryAnnounce && step.decadeIndex >= 0) {
    std::string title = "Decade ";
    title += std::to_string(step.decadeIndex + 1);
    title += " of 5";
    return title;
  }
  return RosaryData::getBeadTypeName(step.type);
}

std::string RosaryPrayerActivity::getStepSubtitle() const {
  auto step = RosaryData::getStep(currentStep);

  if (step.type == RosaryData::BeadType::MysteryAnnounce && step.decadeIndex >= 0) {
    return RosaryData::getMysteryName(mysterySet, step.decadeIndex);
  }

  if (step.type == RosaryData::BeadType::HailMary && step.decadeIndex >= 0) {
    std::string sub = RosaryData::getMysteryName(mysterySet, step.decadeIndex);
    sub += " (";
    sub += std::to_string(step.hailMaryIndex + 1);
    sub += "/10)";
    return sub;
  }

  if (step.type == RosaryData::BeadType::HailMary && step.decadeIndex < 0) {
    std::string sub = "Introductory (";
    sub += std::to_string(step.hailMaryIndex + 1);
    sub += "/3)";
    return sub;
  }

  if (step.type == RosaryData::BeadType::OurFather && step.decadeIndex >= 0) {
    return RosaryData::getMysteryName(mysterySet, step.decadeIndex);
  }

  if (step.type == RosaryData::BeadType::GloryBe && step.decadeIndex >= 0) {
    return RosaryData::getMysteryName(mysterySet, step.decadeIndex);
  }

  if (step.type == RosaryData::BeadType::FatimaPrayer && step.decadeIndex >= 0) {
    return RosaryData::getMysteryName(mysterySet, step.decadeIndex);
  }

  return "";
}

const char* RosaryPrayerActivity::getStepPrayerText() const {
  auto step = RosaryData::getStep(currentStep);

  if (step.type == RosaryData::BeadType::MysteryAnnounce && step.decadeIndex >= 0) {
    // For mystery announcement, show scripture reference
    return RosaryData::getMysteryScripture(mysterySet, step.decadeIndex);
  }

  return RosaryData::Prayers::getPrayerText(step.type);
}

std::string RosaryPrayerActivity::getProgressText() const {
  std::string progress = std::to_string(currentStep + 1);
  progress += "/";
  progress += std::to_string(RosaryData::TOTAL_STEPS);
  return progress;
}

void RosaryPrayerActivity::drawWrappedText(int fontId, int x, int y, int maxWidth, int maxHeight, const char* text,
                                           EpdFontFamily::Style style) const {
  if (!text || text[0] == '\0') return;

  const int lineHeight = renderer.getLineHeight(fontId);
  const int spaceWidth = renderer.getSpaceWidth(fontId);
  int currentX = x;
  int currentY = y;

  const char* wordStart = text;
  const char* pos = text;

  while (*pos != '\0') {
    // Find end of current word
    const char* wordEnd = pos;
    while (*wordEnd != '\0' && *wordEnd != ' ' && *wordEnd != '\n') {
      wordEnd++;
    }

    // Measure this word
    int wordLen = wordEnd - pos;
    if (wordLen > 0) {
      char wordBuf[128];
      int copyLen = wordLen < 127 ? wordLen : 127;
      memcpy(wordBuf, pos, copyLen);
      wordBuf[copyLen] = '\0';

      int wordWidth = renderer.getTextWidth(fontId, wordBuf, style);

      // Check if word fits on current line
      if (currentX > x && (currentX + wordWidth) > (x + maxWidth)) {
        // Move to next line
        currentX = x;
        currentY += lineHeight;
        if (currentY + lineHeight > y + maxHeight) {
          return;  // No more vertical space
        }
      }

      // Draw the word
      renderer.drawText(fontId, currentX, currentY, wordBuf, true, style);
      currentX += wordWidth;
    }

    // Handle separator
    if (*wordEnd == ' ') {
      currentX += spaceWidth;
      pos = wordEnd + 1;
    } else if (*wordEnd == '\n') {
      currentX = x;
      currentY += lineHeight;
      if (currentY + lineHeight > y + maxHeight) {
        return;
      }
      pos = wordEnd + 1;
    } else {
      pos = wordEnd;  // end of string
    }
  }
}

void RosaryPrayerActivity::drawBeadVisualization(int x, int y, int width, int height) const {
  auto step = RosaryData::getStep(currentStep);
  const int beadRadius = 6;
  const int beadSpacing = 4;
  const int beadDiameter = beadRadius * 2;
  const int centerY = y + height / 2;

  if (step.decadeIndex >= 0 && (step.type == RosaryData::BeadType::HailMary ||
                                step.type == RosaryData::BeadType::OurFather ||
                                step.type == RosaryData::BeadType::GloryBe ||
                                step.type == RosaryData::BeadType::FatimaPrayer)) {
    // Draw decade beads: 1 large (Our Father) + 10 small (Hail Marys)
    const int totalBeads = 11;
    const int totalWidth = totalBeads * beadDiameter + (totalBeads - 1) * beadSpacing;
    int startX = x + (width - totalWidth) / 2;

    for (int i = 0; i < totalBeads; i++) {
      int bx = startX + i * (beadDiameter + beadSpacing) + beadRadius;
      int r = (i == 0) ? beadRadius + 2 : beadRadius;

      // Determine if this bead is the current one
      bool isCurrent = false;
      bool isCompleted = false;

      if (i == 0) {
        // Our Father bead
        isCurrent = (step.type == RosaryData::BeadType::OurFather);
        // Our Father is completed if we're past it (on Hail Marys or later)
        isCompleted = (step.type != RosaryData::BeadType::OurFather &&
                       step.type != RosaryData::BeadType::MysteryAnnounce);
      } else {
        // Hail Mary beads (i-1 = 0..9)
        int hmIndex = i - 1;
        isCurrent = (step.type == RosaryData::BeadType::HailMary && step.hailMaryIndex == hmIndex);
        if (step.type == RosaryData::BeadType::HailMary) {
          isCompleted = (hmIndex < step.hailMaryIndex);
        } else if (step.type == RosaryData::BeadType::GloryBe || step.type == RosaryData::BeadType::FatimaPrayer) {
          isCompleted = true;
        }
      }

      if (isCurrent) {
        // Current bead: filled black
        renderer.fillRoundedRect(bx - r, centerY - r, r * 2, r * 2, r, Color::Black);
      } else if (isCompleted) {
        // Completed bead: filled dark gray
        renderer.fillRoundedRect(bx - r, centerY - r, r * 2, r * 2, r, Color::DarkGray);
      } else {
        // Upcoming bead: outline only
        renderer.drawRoundedRect(bx - r, centerY - r, r * 2, r * 2, 1, r, true);
      }
    }

    // Draw decade indicator below beads
    std::string decadeText = "Decade ";
    decadeText += std::to_string(step.decadeIndex + 1);
    decadeText += " of 5";
    int textWidth = renderer.getTextWidth(SMALL_FONT_ID, decadeText.c_str());
    renderer.drawText(SMALL_FONT_ID, x + (width - textWidth) / 2, centerY + beadRadius + 8, decadeText.c_str());
  } else if (step.decadeIndex < 0 && step.type == RosaryData::BeadType::HailMary) {
    // Introductory Hail Marys: show 3 beads
    const int totalBeads = 3;
    const int totalWidth = totalBeads * beadDiameter + (totalBeads - 1) * beadSpacing * 2;
    int startX = x + (width - totalWidth) / 2;

    for (int i = 0; i < totalBeads; i++) {
      int bx = startX + i * (beadDiameter + beadSpacing * 2) + beadRadius;

      bool isCurrent = (step.hailMaryIndex == i);
      bool isCompleted = (i < step.hailMaryIndex);

      if (isCurrent) {
        renderer.fillRoundedRect(bx - beadRadius, centerY - beadRadius, beadDiameter, beadDiameter, beadRadius,
                                 Color::Black);
      } else if (isCompleted) {
        renderer.fillRoundedRect(bx - beadRadius, centerY - beadRadius, beadDiameter, beadDiameter, beadRadius,
                                 Color::DarkGray);
      } else {
        renderer.drawRoundedRect(bx - beadRadius, centerY - beadRadius, beadDiameter, beadDiameter, 1, beadRadius,
                                 true);
      }
    }

    const char* introText = "Introductory Beads";
    int textWidth = renderer.getTextWidth(SMALL_FONT_ID, introText);
    renderer.drawText(SMALL_FONT_ID, x + (width - textWidth) / 2, centerY + beadRadius + 8, introText);
  } else {
    // Non-bead prayer (Creed, Sign of Cross, etc.): show 5 decade overview
    const int dotRadius = 4;
    const int dotSpacing = 12;
    const int totalWidth = 5 * (dotRadius * 2) + 4 * dotSpacing;
    int startX = x + (width - totalWidth) / 2;

    // Determine which decade we're in (or before/after)
    int currentDecade = -1;
    if (step.decadeIndex >= 0) {
      currentDecade = step.decadeIndex;
    } else if (currentStep > 6) {
      // After all decades
      currentDecade = 5;
    }

    for (int i = 0; i < 5; i++) {
      int dx = startX + i * (dotRadius * 2 + dotSpacing) + dotRadius;

      if (i < currentDecade) {
        renderer.fillRoundedRect(dx - dotRadius, centerY - dotRadius, dotRadius * 2, dotRadius * 2, dotRadius,
                                 Color::DarkGray);
      } else if (i == currentDecade) {
        renderer.fillRoundedRect(dx - dotRadius, centerY - dotRadius, dotRadius * 2, dotRadius * 2, dotRadius,
                                 Color::Black);
      } else {
        renderer.drawRoundedRect(dx - dotRadius, centerY - dotRadius, dotRadius * 2, dotRadius * 2, 1, dotRadius,
                                 true);
      }
    }

    const char* overviewText = "Rosary Progress";
    int textWidth = renderer.getTextWidth(SMALL_FONT_ID, overviewText);
    renderer.drawText(SMALL_FONT_ID, x + (width - textWidth) / 2, centerY + dotRadius + 8, overviewText);
  }
}

void RosaryPrayerActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();
  const int sidePadding = metrics.contentSidePadding;

  // --- Header area ---
  // Mystery set name as header
  const char* headerText = RosaryData::getMysterySetName(mysterySet);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerText);

  int contentY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // --- Progress bar ---
  GUI.drawProgressBar(renderer, Rect{sidePadding, contentY, pageWidth - sidePadding * 2, 6}, currentStep + 1,
                      RosaryData::TOTAL_STEPS);
  contentY += 14;

  // --- Step title (prayer type) ---
  std::string title = getStepTitle();
  renderer.drawCenteredText(UI_12_FONT_ID, contentY, title.c_str(), true, EpdFontFamily::BOLD);
  contentY += renderer.getLineHeight(UI_12_FONT_ID) + 4;

  // --- Subtitle (mystery name, bead count, etc.) ---
  std::string subtitle = getStepSubtitle();
  if (!subtitle.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentY, subtitle.c_str(), true, EpdFontFamily::REGULAR);
    contentY += renderer.getLineHeight(UI_10_FONT_ID) + 4;
  }

  // --- Separator line ---
  renderer.drawLine(sidePadding, contentY, pageWidth - sidePadding, contentY);
  contentY += 8;

  // --- Bead visualization ---
  const int beadAreaHeight = 40;
  drawBeadVisualization(sidePadding, contentY, pageWidth - sidePadding * 2, beadAreaHeight);
  contentY += beadAreaHeight + 8;

  // --- Another separator ---
  renderer.drawLine(sidePadding, contentY, pageWidth - sidePadding, contentY);
  contentY += 10;

  // --- Prayer text ---
  const char* prayerText = getStepPrayerText();
  auto step = RosaryData::getStep(currentStep);

  if (step.type == RosaryData::BeadType::MysteryAnnounce && step.decadeIndex >= 0) {
    // For mystery announcement, show the mystery name large and scripture reference
    const char* mysteryName = RosaryData::getMysteryName(mysterySet, step.decadeIndex);
    renderer.drawCenteredText(UI_12_FONT_ID, contentY, mysteryName, true, EpdFontFamily::BOLD);
    contentY += renderer.getLineHeight(UI_12_FONT_ID) + 8;

    // Scripture reference
    std::string scripture = "Scripture: ";
    scripture += prayerText;
    renderer.drawCenteredText(UI_10_FONT_ID, contentY, scripture.c_str(), true, EpdFontFamily::REGULAR);
    contentY += renderer.getLineHeight(UI_10_FONT_ID) + 12;

    // Meditation instruction
    renderer.drawCenteredText(SMALL_FONT_ID, contentY, "Meditate on this mystery", true, EpdFontFamily::REGULAR);
  } else {
    // Show the full prayer text, word-wrapped
    int textAreaHeight = pageHeight - contentY - metrics.buttonHintsHeight - metrics.verticalSpacing;
    drawWrappedText(UI_10_FONT_ID, sidePadding, contentY, pageWidth - sidePadding * 2, textAreaHeight, prayerText);
  }

  // --- Button hints ---
  const char* backLabel = "\x11 Back";
  std::string progressStr = getProgressText();
  const auto labels = mappedInput.mapLabels(backLabel, progressStr.c_str(), "Prev", "Next");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
