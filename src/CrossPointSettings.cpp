#include "CrossPointSettings.h"

#include <HardwareSerial.h>
#include <SD.h>
#include <Serialization.h>

#include <fstream>

constexpr uint8_t SETTINGS_FILE_VERSION = 1;
constexpr char SETTINGS_FILE[] = "/sd/.crosspoint/settings.bin";

bool CrossPointSettings::saveToFile() const {
  // Make sure the directory exists
  SD.mkdir("/.crosspoint");
  
  std::ofstream outputFile(SETTINGS_FILE);
  serialization::writePod(outputFile, SETTINGS_FILE_VERSION);
  serialization::writePod(outputFile, whiteSleepScreen);
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

  serialization::readPod(inputFile, whiteSleepScreen);

  inputFile.close();
  Serial.printf("[%lu] [CPS] Settings loaded from file\n", millis());
  return true;
}