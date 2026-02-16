#include "KoboSyncTokenStore.h"

#include <HalStorage.h>
#include <Logging.h>
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
  if (!Storage.openFileForRead("KTS", KOBO_TOKEN_FILE, file)) {
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
  LOG_DBG("KTS", "Loaded Kobo sync token (%zu chars)", token.size());
  return true;
}

bool KoboSyncTokenStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  FsFile file;
  if (!Storage.openFileForWrite("KTS", KOBO_TOKEN_FILE, file)) {
    return false;
  }

  serialization::writePod(file, KOBO_TOKEN_FILE_VERSION);
  serialization::writeString(file, token);
  file.close();

  LOG_DBG("KTS", "Saved Kobo sync token (%zu chars)", token.size());
  return true;
}

void KoboSyncTokenStore::setToken(const std::string& value) {
  if (value.size() > MAX_TOKEN_LENGTH) {
    token = value.substr(0, MAX_TOKEN_LENGTH);
  } else {
    token = value;
  }
}
