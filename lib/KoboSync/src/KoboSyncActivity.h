#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "activities/ActivityWithSubactivity.h"

/**
 * Kobo/BookLore sync activity: handshake -> library/sync -> download EPUBs to SD.
 * Runs sync steps in loop() to avoid blocking; shows status and completes with a message.
 */
class KoboSyncActivity final : public ActivityWithSubactivity {
 public:
  explicit KoboSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::function<void()>& onDone)
      : ActivityWithSubactivity("KoboSync", renderer, mappedInput), onDone(onDone) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  enum class State {
    IDLE,
    NEED_WIFI,
    HANDSHAKE,
    FETCH_LIST,
    DOWNLOADING,
    DONE,
    ERROR,
  };

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;

  State state = State::IDLE;
  std::string statusMessage;
  std::string errorMessage;
  const std::function<void()> onDone;

  std::string baseUrl;           // https://books.wespo.nl/api/kobo/{token}
  std::string librarySyncUrl;   // from initialization response
  std::vector<std::string> downloadUrls;
  std::vector<std::string> titles;
  size_t downloadIndex = 0;
  bool hasMorePages = true;
  std::string nextSyncToken;  // X-Kobo-SyncToken: empty for first request, then token from response
  size_t totalBookCount = 0;  // from API body when present; 0 = unknown

  // Download progress for progress bar (written from main loop, read by display task)
  size_t downloadDownloaded = 0;
  size_t downloadTotal = 0;

  // Auto-close: when we transition to DONE or ERROR, record time and call onDone() after delay
  unsigned long doneAtMillis = 0;
  static constexpr unsigned long DONE_DISPLAY_MS = 1500;
  static constexpr unsigned long ERROR_DISPLAY_MS = 2000;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void advanceSync();
  void runHandshake();
  void runFetchList();
  void runNextDownload();
  bool ensureWifi();

  // Manifest of synced book paths (/.crosspoint/kobo_synced.txt) for "delete if not on shelf"
  std::vector<std::string> loadManifest() const;
  void saveManifest(const std::vector<std::string>& paths) const;
  void appendToManifest(const std::string& path) const;
  void removeOffShelfBooks();
};
