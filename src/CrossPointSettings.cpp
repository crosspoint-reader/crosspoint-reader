#include "CrossPointSettings.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <cstring>

#include "fontIds.h"

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 2;
// Increment this when adding new persisted settings fields
constexpr uint8_t SETTINGS_COUNT = 19;
constexpr char SETTINGS_FILE[] = "/.crosspoint/settings.bin";
}  // namespace

bool CrossPointSettings::saveToFile() const {
  // Make sure the directory exists
  SdMan.mkdir("/.crosspoint");

  FsFile outputFile;
  if (!SdMan.openFileForWrite("CPS", SETTINGS_FILE, outputFile)) {
    return false;
  }

  serialization::writePod(outputFile, SETTINGS_FILE_VERSION);
  serialization::writePod(outputFile, SETTINGS_COUNT);
  serialization::writePod(outputFile, sleepScreen);
  serialization::writePod(outputFile, extraParagraphSpacing);
  serialization::writePod(outputFile, shortPwrBtn);
  serialization::writePod(outputFile, statusBar);
  serialization::writePod(outputFile, orientation);
  serialization::writePod(outputFile, frontButtonLayout);
  serialization::writePod(outputFile, sideButtonLayout);
  serialization::writePod(outputFile, fontFamily);
  serialization::writePod(outputFile, fontSize);
  serialization::writePod(outputFile, lineSpacing);
  serialization::writePod(outputFile, paragraphAlignment);
  serialization::writePod(outputFile, sleepTimeout);
  serialization::writePod(outputFile, refreshFrequency);
  serialization::writePod(outputFile, screenMargin);
  serialization::writePod(outputFile, sleepScreenCoverMode);
  serialization::writeString(outputFile, std::string(opdsServerUrl));
  serialization::writePod(outputFile, textAntiAliasing);
  serialization::writePod(outputFile, hideBatteryPercentage);
  serialization::writeString(outputFile, std::string(calibreUsername));
  serialization::writeString(outputFile, std::string(calibrePassword));
  outputFile.close();

  Serial.printf("[%lu] [CPS] Settings saved to file\n", millis());
  return true;
}

bool CrossPointSettings::loadFromFile() {
  FsFile inputFile;
  if (!SdMan.openFileForRead("CPS", SETTINGS_FILE, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != SETTINGS_FILE_VERSION) {
    Serial.printf("[%lu] [CPS] Deserialization failed: Unknown version %u\n", millis(), version);
    inputFile.close();
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  // load settings that exist (support older files with fewer fields)
  uint8_t settingsRead = 0;
  do {
    serialization::readPod(inputFile, sleepScreen);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, shortPwrBtn);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, statusBar);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, orientation);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, frontButtonLayout);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, sideButtonLayout);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fontFamily);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fontSize);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, lineSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, paragraphAlignment);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, sleepTimeout);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, refreshFrequency);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, sleepScreenCoverMode);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(opdsServerUrl, urlStr.c_str(), sizeof(opdsServerUrl) - 1);
      opdsServerUrl[sizeof(opdsServerUrl) - 1] = '\0';
    }
    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, hideBatteryPercentage);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(calibreUsername, usernameStr.c_str(), sizeof(calibreUsername) - 1);
      calibreUsername[sizeof(calibreUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(calibrePassword, passwordStr.c_str(), sizeof(calibrePassword) - 1);
      calibrePassword[sizeof(calibrePassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
  } while (false);

  inputFile.close();
  Serial.printf("[%lu] [CPS] Settings loaded from file\n", millis());
  return true;
}

float CrossPointSettings::getReaderLineCompression() const {
  switch (fontFamily) {
    case BOOKERLY:
    default:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.1f;
      }
    case NOTOSANS:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
    case OPENDYSLEXIC:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  switch (sleepTimeout) {
    case SLEEP_1_MIN:
      return 1UL * 60 * 1000;
    case SLEEP_5_MIN:
      return 5UL * 60 * 1000;
    case SLEEP_10_MIN:
    default:
      return 10UL * 60 * 1000;
    case SLEEP_15_MIN:
      return 15UL * 60 * 1000;
    case SLEEP_30_MIN:
      return 30UL * 60 * 1000;
  }
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
}

int CrossPointSettings::getReaderFontId() const {
  switch (fontFamily) {
    case BOOKERLY:
    default:
      switch (fontSize) {
        case SMALL:
          return BOOKERLY_12_FONT_ID;
        case MEDIUM:
        default:
          return BOOKERLY_14_FONT_ID;
        case LARGE:
          return BOOKERLY_16_FONT_ID;
        case EXTRA_LARGE:
          return BOOKERLY_18_FONT_ID;
      }
    case NOTOSANS:
      switch (fontSize) {
        case SMALL:
          return NOTOSANS_12_FONT_ID;
        case MEDIUM:
        default:
          return NOTOSANS_14_FONT_ID;
        case LARGE:
          return NOTOSANS_16_FONT_ID;
        case EXTRA_LARGE:
          return NOTOSANS_18_FONT_ID;
      }
    case OPENDYSLEXIC:
      switch (fontSize) {
        case SMALL:
          return OPENDYSLEXIC_8_FONT_ID;
        case MEDIUM:
        default:
          return OPENDYSLEXIC_10_FONT_ID;
        case LARGE:
          return OPENDYSLEXIC_12_FONT_ID;
        case EXTRA_LARGE:
          return OPENDYSLEXIC_14_FONT_ID;
      }
  }
}
