#include "CrossPointSettings.h"

#include <HardwareSerial.h>
#include <SD.h>
#include <Serialization.h>

#include <cstdint>
#include <fstream>

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 1;
constexpr uint8_t SETTINGS_COUNT = 3;
constexpr char SETTINGS_FILE[] = "/sd/.crosspoint/settings.bin";
}  // namespace

bool CrossPointSettings::saveToFile() const {
  // Make sure the directory exists
  SD.mkdir("/.crosspoint");

  std::ofstream outputFile(SETTINGS_FILE);
  serialization::writePod(outputFile, SETTINGS_FILE_VERSION);
  serialization::writePod(outputFile, SETTINGS_COUNT);
  serialization::writePod(outputFile, sleepScreen);
  serialization::writePod(outputFile, extraParagraphSpacing);
  serialization::writePod(outputFile, shortPwrBtn);
  outputFile.close();

  Serial.printf("[%lu] [CPS] Settings saved to file\n", millis());
  return true;
}

bool CrossPointSettings::loadFromFile() {
  if (!SD.exists(SETTINGS_FILE + 3)) {  // +3 to skip "/sd" prefix
    Serial.printf("[%lu] [CPS] Settings file does not exist, using defaults\n", millis());
    return false;
  }

  std::ifstream inputFile(SETTINGS_FILE);

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != SETTINGS_FILE_VERSION) {
    Serial.printf("[%lu] [CPS] Deserialization failed: Unknown version %u\n", millis(), version);
    inputFile.close();
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  // load settings that exist
  uint8_t settingsRead = 0;
  do {
    serialization::readPod(inputFile, sleepScreen);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, shortPwrBtn);
    if (++settingsRead >= fileSettingsCount) break;
  } while (false);

  inputFile.close();
  Serial.printf("[%lu] [CPS] Settings loaded from file\n", millis());
  return true;
}
