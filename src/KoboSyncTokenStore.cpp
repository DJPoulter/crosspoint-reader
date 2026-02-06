#include "KoboSyncTokenStore.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <cstring>

namespace {
constexpr uint8_t KOBO_TOKEN_FILE_VERSION = 1;
constexpr char KOBO_TOKEN_FILE[] = "/.crosspoint/kobosync.bin";
constexpr size_t MAX_TOKEN_LENGTH = 63;
}  // namespace

KoboSyncTokenStore KoboSyncTokenStore::instance;

bool KoboSyncTokenStore::loadFromFile() {
  FsFile file;
  if (!SdMan.openFileForRead("KTS", KOBO_TOKEN_FILE, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != KOBO_TOKEN_FILE_VERSION) {
    file.close();
    return false;
  }

  serialization::readString(file, token);
  file.close();

  if (token.size() > MAX_TOKEN_LENGTH) {
    token.resize(MAX_TOKEN_LENGTH);
  }
  Serial.printf("[%lu] [KTS] Loaded Kobo sync token (%zu chars)\n", millis(), token.size());
  return true;
}

bool KoboSyncTokenStore::saveToFile() const {
  SdMan.mkdir("/.crosspoint");

  FsFile file;
  if (!SdMan.openFileForWrite("KTS", KOBO_TOKEN_FILE, file)) {
    return false;
  }

  serialization::writePod(file, KOBO_TOKEN_FILE_VERSION);
  serialization::writeString(file, token);
  file.close();

  Serial.printf("[%lu] [KTS] Saved Kobo sync token (%zu chars)\n", millis(), token.size());
  return true;
}

void KoboSyncTokenStore::setToken(const std::string& value) {
  if (value.size() > MAX_TOKEN_LENGTH) {
    token = value.substr(0, MAX_TOKEN_LENGTH);
  } else {
    token = value;
  }
}
