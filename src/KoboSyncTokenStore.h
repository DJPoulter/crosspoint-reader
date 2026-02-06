#pragma once

#include <string>

/**
 * Singleton store for the Kobo/BookLore sync token.
 * Stored in /.crosspoint/kobosync.bin (separate from settings.bin).
 */
class KoboSyncTokenStore {
 private:
  static KoboSyncTokenStore instance;
  std::string token;

  KoboSyncTokenStore() = default;

 public:
  KoboSyncTokenStore(const KoboSyncTokenStore&) = delete;
  KoboSyncTokenStore& operator=(const KoboSyncTokenStore&) = delete;

  static KoboSyncTokenStore& getInstance() { return instance; }

  bool loadFromFile();
  bool saveToFile() const;

  const std::string& getToken() const { return token; }
  void setToken(const std::string& value);
};

#define KOBO_TOKEN_STORE KoboSyncTokenStore::getInstance()
