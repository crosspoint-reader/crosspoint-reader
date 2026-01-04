#include "CrossPointState.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 3;
constexpr char STATE_FILE[] = "/.crosspoint/state.bin";
}  // namespace

CrossPointState CrossPointState::instance;

bool CrossPointState::saveToFile() const {
  FsFile outputFile;
  if (!SdMan.openFileForWrite("CPS", STATE_FILE, outputFile)) {
    return false;
  }

  serialization::writePod(outputFile, STATE_FILE_VERSION);
  serialization::writeString(outputFile, openEpubPath);
  serialization::writeString(outputFile, lastBrowsedFolder);
  serialization::writePod(outputFile, lastScheduledServerTime);
  outputFile.close();
  return true;
}

bool CrossPointState::loadFromFile() {
  FsFile inputFile;
  if (!SdMan.openFileForRead("CPS", STATE_FILE, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version == 1) {
    // Version 1: only had openEpubPath
    serialization::readString(inputFile, openEpubPath);
    lastScheduledServerTime = 0;
  } else if (version == 2) {
    // Version 2: has openEpubPath and lastBrowsedFolder
    serialization::readString(inputFile, openEpubPath);
    serialization::readString(inputFile, lastBrowsedFolder);
    lastScheduledServerTime = 0;
  } else if (version == STATE_FILE_VERSION) {
    // Version 3: has openEpubPath, lastBrowsedFolder, and lastScheduledServerTime
    serialization::readString(inputFile, openEpubPath);
    serialization::readString(inputFile, lastBrowsedFolder);
    serialization::readPod(inputFile, lastScheduledServerTime);
  } else {
    Serial.printf("[%lu] [CPS] Deserialization failed: Unknown version %u\n", millis(), version);
    inputFile.close();
    return false;
  }

  inputFile.close();
  return true;
}
