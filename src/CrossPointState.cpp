#include "CrossPointState.h"

#include <FsHelpers.h>
#include <HardwareSerial.h>
#include <Serialization.h>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 1;
constexpr char STATE_FILE[] = "/.crosspoint/state.bin";
}  // namespace

CrossPointState CrossPointState::instance;

bool CrossPointState::saveToFile() const {
  File outputFile;
  if (!FsHelpers::openFileForWrite("CPS", STATE_FILE, outputFile)) {
    return false;
  }

  serialization::writePod(outputFile, STATE_FILE_VERSION);
  serialization::writeString(outputFile, openEpubPath);
  outputFile.close();
  return true;
}

bool CrossPointState::loadFromFile() {
  File inputFile;
  if (!FsHelpers::openFileForRead("CPS", STATE_FILE, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != STATE_FILE_VERSION) {
    Serial.printf("[%lu] [CPS] Deserialization failed: Unknown version %u\n", millis(), version);
    inputFile.close();
    return false;
  }

  serialization::readString(inputFile, openEpubPath);

  inputFile.close();
  return true;
}
