#include "KoboSyncActivity.h"

#include <ArduinoJson.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <set>
#include <cstring>

#include <SdFat.h>
#include <Serialization.h>

#include "KoboSyncTokenStore.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"

namespace {
constexpr char KOBO_BASE_HOST[] = "https://books.wespo.nl/api/kobo/";
// Kobo-like User-Agent so BookLore/Kobo APIs accept requests (avoid 403)
constexpr char KOBO_USER_AGENT[] = "Kobo eReader";
constexpr char KOBO_SYNCED_MANIFEST[] = "/.crosspoint/kobo_synced.bin";
constexpr uint8_t KOBO_SYNCED_VERSION = 1;
constexpr uint32_t KOBO_SYNCED_MAX_ENTRIES = 500;  // cap when reading to avoid corrupt file
}  // namespace

void KoboSyncActivity::taskTrampoline(void* param) {
  auto* self = static_cast<KoboSyncActivity*>(param);
  self->displayTaskLoop();
}

void KoboSyncActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  state = State::IDLE;
  statusMessage = "Starting...";
  errorMessage.clear();
  baseUrl = std::string(KOBO_BASE_HOST) + KOBO_TOKEN_STORE.getToken();
  librarySyncUrl.clear();
  nextSyncToken.clear();
  downloadUrls.clear();
  titles.clear();
  downloadIndex = 0;
  hasMorePages = true;
  totalBookCount = 0;
  updateRequired = true;

  // Always start display task so the UI is visible
  xTaskCreate(&KoboSyncActivity::taskTrampoline, "KoboSyncTask", 4096, this, 1, &displayTaskHandle);

  // Use system WiFi flow (WifiSelectionActivity auto-connects to last-used network on entry)
  if (!ensureWifi()) {
    state = State::NEED_WIFI;
    statusMessage = "Choose a network";
    updateRequired = true;
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput, [this](bool connected) {
      exitActivity();
      if (connected) {
        state = State::HANDSHAKE;
        statusMessage = "Connecting...";
      } else {
        state = State::ERROR;
        errorMessage = "WiFi cancelled";
        doneAtMillis = millis();
      }
      updateRequired = true;
    }));
    return;
  }

  state = State::HANDSHAKE;
  statusMessage = "Connecting...";
  updateRequired = true;
}

void KoboSyncActivity::onExit() {
  // Disconnect WiFi after sync so we don't leave the radio on
  if (state == State::DONE || state == State::ERROR) {
    WiFi.disconnect(false);
    LOG_DBG("Kobo", "WiFi disconnected after sync");
  }
  Activity::onExit();
  if (renderingMutex) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (displayTaskHandle) {
      vTaskDelete(displayTaskHandle);
      displayTaskHandle = nullptr;
    }
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }
}

bool KoboSyncActivity::ensureWifi() {
  return (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0));
}

void KoboSyncActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == State::NEED_WIFI) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onDone();
    }
    return;
  }

  if (state == State::DONE || state == State::ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onDone();
      return;
    }
    const unsigned long elapsed = millis() - doneAtMillis;
    if (state == State::DONE && elapsed >= DONE_DISPLAY_MS) {
      onDone();
      return;
    }
    if (state == State::ERROR && elapsed >= ERROR_DISPLAY_MS) {
      onDone();
      return;
    }
    return;
  }

  advanceSync();
}

void KoboSyncActivity::advanceSync() {
  switch (state) {
    case State::HANDSHAKE:
      runHandshake();
      break;
    case State::FETCH_LIST:
      runFetchList();
      break;
    case State::DOWNLOADING:
      runNextDownload();
      break;
    default:
      break;
  }
}

void KoboSyncActivity::runHandshake() {
  const std::string url = baseUrl + "/v1/initialization";
  std::string body;
  if (!HttpDownloader::fetchUrlNoAuth(url, body, KOBO_USER_AGENT)) {
    state = State::ERROR;
    errorMessage = "Handshake failed";
    doneAtMillis = millis();
    updateRequired = true;
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    state = State::ERROR;
    errorMessage = "Invalid response";
    doneAtMillis = millis();
    updateRequired = true;
    return;
  }

  // BookLore / Kobo APIs may use "library_sync", "librarySync", or nest under "Resources"
  if (doc["library_sync"].is<const char*>()) {
    librarySyncUrl = doc["library_sync"].as<std::string>();
  } else if (doc["librarySync"].is<const char*>()) {
    librarySyncUrl = doc["librarySync"].as<std::string>();
  } else if (doc["Resources"]["library_sync"].is<const char*>()) {
    librarySyncUrl = doc["Resources"]["library_sync"].as<std::string>();
  } else if (doc["Resources"]["librarySync"].is<const char*>()) {
    librarySyncUrl = doc["Resources"]["librarySync"].as<std::string>();
  }

  if (librarySyncUrl.empty()) {
    LOG_ERR("Kobo", "Init response (%zu bytes), no library_sync URL", body.size());
    if (body.size() > 0 && body.size() <= 256) {
      LOG_DBG("Kobo", "Body: %s", body.c_str());
    } else if (body.size() > 256) {
      LOG_DBG("Kobo", "Body (first 200): %.200s", body.c_str());
    }
    state = State::ERROR;
    errorMessage = "Invalid response";
    doneAtMillis = millis();
    updateRequired = true;
    return;
  }

  LOG_DBG("Kobo", "library_sync: %s", librarySyncUrl.c_str());
  hasMorePages = true;
  state = State::FETCH_LIST;
  statusMessage = "Fetching list...";
  updateRequired = true;
}

void KoboSyncActivity::runFetchList() {
  // Do NOT use ?page= or ?pageSize=. The server ignores them and uses X-Kobo-SyncToken.
  const std::string url = librarySyncUrl;
  const std::string requestToken = nextSyncToken;

  HttpDownloader::KoboSyncHeaders headers;
  headers.requestSyncToken = requestToken;

  std::string body;
  JsonDocument doc;
  DeserializationError err;
  constexpr int maxAttempts = 3;

  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    if (attempt > 0) {
      LOG_DBG("Kobo", "Retry %d/%d after truncated response (IncompleteInput)", attempt, maxAttempts);
    }
    body.clear();
    if (!HttpDownloader::fetchUrlNoAuthKoboSync(url, body, KOBO_USER_AGENT, &headers)) {
      state = State::ERROR;
      errorMessage = "List fetch failed";
      doneAtMillis = millis();
      updateRequired = true;
      return;
    }

    nextSyncToken = headers.responseSyncToken;
    hasMorePages = (headers.responseSync == "continue");

    err = deserializeJson(doc, body);
    if (!err) {
      break;
    }

    LOG_ERR("Kobo", "JSON error: %s, body size %zu", err.c_str(), body.size());
    if (body.size() > 0 && body.size() <= 300) {
      LOG_DBG("Kobo", "Body: %s", body.c_str());
    } else if (body.size() > 300) {
      LOG_DBG("Kobo", "Body (first 300): %.300s", body.c_str());
    }

    // Retry only on IncompleteInput (truncated body); fail immediately on other errors or after last attempt
    if (strcmp(err.c_str(), "IncompleteInput") != 0 || attempt + 1 >= maxAttempts) {
      state = State::ERROR;
      errorMessage = "Invalid list JSON";
      doneAtMillis = millis();
      updateRequired = true;
      return;
    }
  }

  // BookLore / Kobo library/sync returns a root-level array or { "Items": [...] }
  JsonArray items = doc.as<JsonArray>();
  if (items.isNull()) {
    items = doc["Items"].as<JsonArray>();
  }
  if (items.isNull()) {
    items = doc["items"].as<JsonArray>();
  }
  if (items.isNull() || items.size() == 0) {
    hasMorePages = false;
    removeOffShelfBooks();  // full list fetched (empty page = no more); remove books no longer on shelf
    if (downloadUrls.empty()) {
      state = State::DONE;
      statusMessage = "No new books";
      doneAtMillis = millis();
    } else {
      state = State::DOWNLOADING;
      downloadIndex = 0;
      statusMessage = "Downloading...";
    }
    updateRequired = true;
    return;
  }

  for (size_t i = 0; i < items.size(); i++) {
    JsonObject item = items[i].as<JsonObject>();
    const char* title = nullptr;
    const char* downloadUrl = nullptr;

    JsonObject newEnt = item["NewEntitlement"].as<JsonObject>();
    if (!newEnt.isNull()) {
      JsonObject meta = newEnt["BookMetadata"].as<JsonObject>();
      if (!meta.isNull()) {
        if (meta["Title"].is<const char*>()) {
          title = meta["Title"].as<const char*>();
        }
        JsonArray urls = meta["DownloadUrls"].as<JsonArray>();
        if (!urls.isNull() && urls.size() > 0) {
          for (size_t u = 0; u < urls.size(); u++) {
            JsonObject entry = urls[u].as<JsonObject>();
            const char* fmt = entry["Format"].as<const char*>();
            if (fmt && (strcmp(fmt, "EPUB") == 0 || strcmp(fmt, "EPUB3") == 0)) {
              if (entry["Url"].is<const char*>()) {
                downloadUrl = entry["Url"].as<const char*>();
                break;
              }
            }
          }
          if (!downloadUrl && urls[0]["Url"].is<const char*>()) {
            downloadUrl = urls[0]["Url"].as<const char*>();
          }
        }
      }
    }

    if (!title && item["Metadata"]["Title"].is<const char*>()) {
      title = item["Metadata"]["Title"].as<const char*>();
    }
    if (!title && item["Title"].is<const char*>()) {
      title = item["Title"].as<const char*>();
    }
    if (!downloadUrl && item["DownloadUrl"].is<const char*>()) {
      downloadUrl = item["DownloadUrl"].as<const char*>();
    }

    if (!downloadUrl || downloadUrl[0] == '\0') {
      continue;
    }

    const std::string titleStr = title ? title : "Book";
    titles.push_back(titleStr);
    downloadUrls.push_back(downloadUrl);
  }

  LOG_DBG("Kobo", "Token response: %zu items (total %zu), X-Kobo-Sync=%s, token_len=%zu", items.size(),
          downloadUrls.size(), headers.responseSync.c_str(), nextSyncToken.size());
  // Fallback: server may send next token but not X-Kobo-Sync; if we got items and have a token, request again
  if (!hasMorePages && !nextSyncToken.empty() && items.size() > 0) {
    hasMorePages = true;
    LOG_DBG("Kobo", "More pages (token present, X-Kobo-Sync missing)");
  }

  // Optional: total from body for display (server may not send it when using token pagination)
  if (totalBookCount == 0 && doc.is<JsonObject>()) {
    auto getCount = [&doc](const char* key) -> size_t {
      if (!doc[key].is<int>() && !doc[key].is<long>() && !doc[key].is<unsigned int>()) return 0;
      long v = doc[key].as<long>();
      return (v > 0) ? static_cast<size_t>(v) : 0;
    };
    totalBookCount = getCount("TotalCount");
    if (totalBookCount == 0) totalBookCount = getCount("totalCount");
    if (totalBookCount == 0) totalBookCount = getCount("Count");
    if (totalBookCount == 0) totalBookCount = getCount("Total");
    if (totalBookCount == 0) totalBookCount = getCount("total");
    if (totalBookCount == 0) totalBookCount = getCount("NumberOfItems");
    if (totalBookCount > 0) {
      LOG_DBG("Kobo", "API reports %zu books total", totalBookCount);
    }
  }

  if (!hasMorePages) {
    removeOffShelfBooks();
    if (downloadUrls.empty()) {
      state = State::DONE;
      statusMessage = "No new books";
      doneAtMillis = millis();
    } else {
      state = State::DOWNLOADING;
      downloadIndex = 0;
      statusMessage = "Downloading...";
    }
  } else {
    if (totalBookCount > 0) {
      statusMessage =
          "Fetching list... " + std::to_string(downloadUrls.size()) + " / " + std::to_string(totalBookCount);
    } else {
      statusMessage = "Fetching list... " + std::to_string(downloadUrls.size()) + " so far";
    }
  }
  updateRequired = true;
}

void KoboSyncActivity::runNextDownload() {
  if (downloadIndex >= downloadUrls.size()) {
    state = State::DONE;
    statusMessage = "Sync complete";
    doneAtMillis = millis();
    updateRequired = true;
    return;
  }

  const std::string& url = downloadUrls[downloadIndex];
  const std::string& title = titles[downloadIndex];
  if (url.empty()) {
    downloadIndex++;
    updateRequired = true;
    return;
  }

  std::string filename = "/" + StringUtils::sanitizeFilename(title, 80) + ".epub";

  if (Storage.exists(filename.c_str())) {
    appendToManifest(filename);
    downloadIndex++;
    if (downloadIndex >= downloadUrls.size()) {
      state = State::DONE;
      statusMessage = "Sync complete";
      doneAtMillis = millis();
    }
    updateRequired = true;
    return;
  }

  statusMessage = "Downloading: " + title;
  if (statusMessage.length() > 35) {
    statusMessage = statusMessage.substr(0, 32) + "...";
  }
  downloadDownloaded = 0;
  downloadTotal = 0;
  updateRequired = true;
  // Let display task render "Downloading" + empty bar before we block (so bar is always visible)
  for (int i = 0; i < 15; i++) {
    vTaskDelay(1);
  }

  const auto result = HttpDownloader::downloadToFileNoAuth(
      url, filename,
      [this](size_t downloaded, size_t total) {
        this->downloadDownloaded = downloaded;
        this->downloadTotal = total;
        this->updateRequired = true;
      },
      KOBO_USER_AGENT);

  if (result != HttpDownloader::OK) {
    state = State::ERROR;
    errorMessage = "Download failed: " + title;
    doneAtMillis = millis();
    updateRequired = true;
    return;
  }

  appendToManifest(filename);
  downloadIndex++;
  if (downloadIndex >= downloadUrls.size()) {
    state = State::DONE;
    statusMessage = "Sync complete";
    doneAtMillis = millis();
  }
  updateRequired = true;
}

void KoboSyncActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);  // same as OPDS for responsive progress bar
  }
}

void KoboSyncActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 20, "Kobo Sync", true, EpdFontFamily::BOLD);

  const int msgY = 80;
  if (state == State::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, msgY, errorMessage.c_str(), true);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, msgY, statusMessage.c_str(), true);
  }

  // Downloading: match OPDS layout - status text then progress bar (always show bar so it's visible from first frame)
  if (state == State::DOWNLOADING && downloadIndex < downloadUrls.size()) {
    const std::string progress =
        std::to_string(downloadIndex + 1) + " / " + std::to_string(downloadUrls.size());
    renderer.drawCenteredText(UI_10_FONT_ID, msgY + 30, progress.c_str(), true);
    size_t barCurrent = downloadDownloaded;
    size_t barTotal = downloadTotal;
    if (barTotal == 0 && barCurrent > 0) {
      constexpr size_t kIndeterminateMax = 1024 * 1024;
      barTotal = kIndeterminateMax;
      if (barCurrent > kIndeterminateMax) barCurrent = kIndeterminateMax;
    }
    if (barTotal == 0 && barCurrent == 0) {
      barTotal = 1;  // show empty bar (0%) so bar is visible before first chunk
    }
    const int barY = msgY + 55;
    const int barW = pageWidth - 40;
    const int barH = 24;
    GUI.drawProgressBar(renderer, Rect{20, barY, barW, barH}, barCurrent, barTotal);
  }

  const char* btn2 = (state == State::DONE || state == State::ERROR) ? "OK" : "";
  const auto labels = mappedInput.mapLabels("Back", btn2, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

std::vector<std::string> KoboSyncActivity::loadManifest() const {
  std::vector<std::string> paths;
  FsFile file;
  if (!Storage.openFileForRead("KSA", KOBO_SYNCED_MANIFEST, file)) {
    return paths;
  }
  uint8_t version;
  serialization::readPod(file, version);
  if (version != KOBO_SYNCED_VERSION) {
    file.close();
    return paths;
  }
  uint32_t count;
  serialization::readPod(file, count);
  if (count > KOBO_SYNCED_MAX_ENTRIES) {
    count = KOBO_SYNCED_MAX_ENTRIES;
  }
  paths.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    std::string path;
    serialization::readString(file, path);
    if (!path.empty()) {
      paths.push_back(std::move(path));
    }
  }
  file.close();
  return paths;
}

void KoboSyncActivity::saveManifest(const std::vector<std::string>& paths) const {
  Storage.mkdir("/.crosspoint");
  FsFile file;
  if (!Storage.openFileForWrite("KSA", KOBO_SYNCED_MANIFEST, file)) {
    return;
  }
  serialization::writePod(file, KOBO_SYNCED_VERSION);
  const uint32_t count = static_cast<uint32_t>(paths.size());
  serialization::writePod(file, count);
  for (const auto& p : paths) {
    serialization::writeString(file, p);
  }
  file.close();
}

void KoboSyncActivity::appendToManifest(const std::string& path) const {
  std::vector<std::string> paths = loadManifest();
  if (std::find(paths.begin(), paths.end(), path) != paths.end()) {
    return;
  }
  paths.push_back(path);
  saveManifest(paths);
}

void KoboSyncActivity::removeOffShelfBooks() {
  std::set<std::string> shelfPaths;
  for (const auto& title : titles) {
    shelfPaths.insert("/" + StringUtils::sanitizeFilename(title, 80) + ".epub");
  }
  std::vector<std::string> manifest = loadManifest();
  LOG_DBG("Kobo", "removeOffShelfBooks: manifest %zu, shelf %zu", manifest.size(), shelfPaths.size());

  std::vector<std::string> kept;
  for (const auto& path : manifest) {
    // Normalize: manifest may have been written with or without leading slash
    const std::string norm = (path.empty() || path[0] != '/') ? ("/" + path) : path;

    if (shelfPaths.count(norm) != 0) {
      kept.push_back(norm);
    } else {
      if (Storage.exists(norm.c_str())) {
        if (Storage.remove(norm.c_str())) {
          LOG_DBG("Kobo", "Removed (no longer on shelf): %s", norm.c_str());
        } else {
          LOG_ERR("Kobo", "Remove failed (file in use?): %s", norm.c_str());
        }
      }
      // Path no longer on shelf: do not add to kept so manifest is updated below
    }
  }
  if (kept.size() != manifest.size()) {
    saveManifest(kept);
  }
}
