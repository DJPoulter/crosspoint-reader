#include "CrossPointState.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 2;  // Bumped to 2 to add wasOnBook field
constexpr char STATE_FILE[] = "/.crosspoint/state.bin";
}  // namespace

CrossPointState CrossPointState::instance;

bool CrossPointState::saveToFile() const {
  FsFile outputFile;
  if (!SdMan.openFileForWrite("CPS", STATE_FILE, outputFile)) {
    Serial.printf("[%lu] [CPS] Failed to open state file for writing\n", millis());
    return false;
  }

  serialization::writePod(outputFile, STATE_FILE_VERSION);
  serialization::writeString(outputFile, openEpubPath);
  serialization::writePod(outputFile, wasOnBook);
  outputFile.close();
  Serial.printf("[%lu] [CPS] Saved state: openEpubPath='%s', wasOnBook=%d\n", millis(), 
                openEpubPath.c_str(), wasOnBook);
  return true;
}

bool CrossPointState::loadFromFile() {
  FsFile inputFile;
  if (!SdMan.openFileForRead("CPS", STATE_FILE, inputFile)) {
    Serial.printf("[%lu] [CPS] State file not found, using defaults\n", millis());
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  Serial.printf("[%lu] [CPS] Loading state file version %u\n", millis(), version);
  if (version > STATE_FILE_VERSION) {
    Serial.printf("[%lu] [CPS] Deserialization failed: Unknown version %u (expected <= %u)\n", 
                  millis(), version, STATE_FILE_VERSION);
    inputFile.close();
    return false;
  }

  serialization::readString(inputFile, openEpubPath);
  
  // Read wasOnBook if version >= 2, otherwise default to false
  if (version >= 2) {
    serialization::readPod(inputFile, wasOnBook);
    Serial.printf("[%lu] [CPS] Loaded state: openEpubPath='%s', wasOnBook=%d\n", millis(), 
                  openEpubPath.c_str(), wasOnBook);
  } else {
    wasOnBook = false;  // Default for old state files
    Serial.printf("[%lu] [CPS] Loaded state (v1): openEpubPath='%s', wasOnBook=false (default)\n", 
                  millis(), openEpubPath.c_str());
  }

  inputFile.close();
  return true;
}
