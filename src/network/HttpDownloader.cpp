#include "HttpDownloader.h"

#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <StreamString.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <freertos/task.h>

#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "util/UrlUtils.h"

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent) {
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
  }
  HTTPClient http;

  Serial.printf("[%lu] [HTTP] Fetching: %s\n", millis(), url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  // Add Basic HTTP auth if credentials are configured
  if (strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [HTTP] Fetch failed: %d\n", millis(), httpCode);
    http.end();
    return false;
  }

  http.writeToStream(&outContent);

  http.end();

  Serial.printf("[%lu] [HTTP] Fetch success\n", millis());
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent) {
  StreamString stream;
  if (!fetchUrl(url, stream)) {
    return false;
  }
  outContent = stream.c_str();
  return true;
}

bool HttpDownloader::fetchUrlNoAuth(const std::string& url, std::string& outContent, const char* userAgent) {
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
  }
  HTTPClient http;

  Serial.printf("[%lu] [HTTP] Fetch (no auth): %s\n", millis(), url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (userAgent != nullptr) {
    http.addHeader("User-Agent", userAgent);
  } else {
    http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  }
  http.setTimeout(20000);  // 20 s so sync doesn't block indefinitely

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [HTTP] Fetch failed: %d\n", millis(), httpCode);
    http.end();
    return false;
  }

  outContent = http.getString().c_str();
  http.end();
  Serial.printf("[%lu] [HTTP] Fetch success\n", millis());
  return true;
}

bool HttpDownloader::fetchUrlNoAuthKoboSync(const std::string& url, std::string& outContent,
                                            const char* userAgent, KoboSyncHeaders* koboHeaders) {
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
  }
  HTTPClient http;

  Serial.printf("[%lu] [HTTP] Fetch (Kobo sync): %s\n", millis(), url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (userAgent != nullptr) {
    http.addHeader("User-Agent", userAgent);
  } else {
    http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  }
  // 60s: paginated sync responses can be large/slow; 20s caused truncated body (IncompleteInput) on second page
  http.setTimeout(5000);
  if (koboHeaders != nullptr && !koboHeaders->requestSyncToken.empty()) {
    http.addHeader("X-Kobo-SyncToken", koboHeaders->requestSyncToken.c_str());
  }
  // ESP32 HTTPClient does not store response headers unless we ask to collect them first
  if (koboHeaders != nullptr) {
    const char* koboHeaderKeys[] = {"X-Kobo-Sync", "X-Kobo-sync", "x-kobo-sync", "X-Kobo-SyncToken", "x-kobo-synctoken"};
    http.collectHeaders(koboHeaderKeys, 5);
  }

  const unsigned long t0 = millis();
  const int httpCode = http.GET();
  const unsigned long afterGet = millis();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [HTTP] Fetch failed: %d\n", millis(), httpCode);
    http.end();
    return false;
  }

  const int contentLength = static_cast<int>(http.getSize());  // -1 if chunked/missing

  if (koboHeaders != nullptr) {
    // Server (BookLore) sends "X-Kobo-sync" and "x-kobo-synctoken"; try exact and variants (lookup may be case-sensitive)
    String tok = http.header("X-Kobo-SyncToken");
    if (tok.length() == 0) tok = http.header("x-kobo-synctoken");
    if (tok.length() == 0) tok = http.header("X-Kobo-SyncToken");
    koboHeaders->responseSyncToken = tok.c_str();
    String sync = http.header("X-Kobo-Sync");
    if (sync.length() == 0) sync = http.header("x-kobo-sync");
    if (sync.length() == 0) sync = http.header("X-Kobo-sync");  // BookLore sends this exact casing
    koboHeaders->responseSync = sync.c_str();
  }
  outContent = http.getString().c_str();
  http.end();
  const unsigned long afterBody = millis();
  // Diagnose slow/truncated sync: GET = time to first byte (server/network); body = read time.
  // If body time ~timeout and size < Content-Length: server sent partial then stalled, or read timeout.
  Serial.printf("[%lu] [HTTP] Fetch success (GET %lu ms, body %lu ms) size %zu%s\n", millis(),
                afterGet - t0, afterBody - afterGet, outContent.size(),
                (contentLength > 0 && outContent.size() != static_cast<size_t>(contentLength))
                    ? " TRUNCATED"
                    : "");
  if (contentLength > 0 && outContent.size() != static_cast<size_t>(contentLength)) {
    Serial.printf("[%lu] [HTTP] Expected Content-Length: %d\n", millis(), contentLength);
  }
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress) {
  // Use WiFiClientSecure for HTTPS, regular WiFiClient for HTTP
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
  }
  HTTPClient http;

  Serial.printf("[%lu] [HTTP] Downloading: %s\n", millis(), url.c_str());
  Serial.printf("[%lu] [HTTP] Destination: %s\n", millis(), destPath.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  // Add Basic HTTP auth if credentials are configured
  if (strlen(SETTINGS.opdsUsername) > 0 && strlen(SETTINGS.opdsPassword) > 0) {
    std::string credentials = std::string(SETTINGS.opdsUsername) + ":" + SETTINGS.opdsPassword;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [HTTP] Download failed: %d\n", millis(), httpCode);
    http.end();
    return HTTP_ERROR;
  }

  const size_t contentLength = http.getSize();
  Serial.printf("[%lu] [HTTP] Content-Length: %zu\n", millis(), contentLength);

  // Remove existing file if present
  if (SdMan.exists(destPath.c_str())) {
    SdMan.remove(destPath.c_str());
  }

  // Open file for writing
  FsFile file;
  if (!SdMan.openFileForWrite("HTTP", destPath.c_str(), file)) {
    Serial.printf("[%lu] [HTTP] Failed to open file for writing\n", millis());
    http.end();
    return FILE_ERROR;
  }

  // Get the stream for chunked reading
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    Serial.printf("[%lu] [HTTP] Failed to get stream\n", millis());
    file.close();
    SdMan.remove(destPath.c_str());
    http.end();
    return HTTP_ERROR;
  }

  // Download in chunks (8KB for throughput, same as downloadToFileNoAuth)
  uint8_t buffer[DOWNLOAD_CHUNK_SIZE];
  size_t downloaded = 0;
  const size_t total = contentLength > 0 ? contentLength : 0;

  while (http.connected() && (contentLength == 0 || downloaded < contentLength)) {
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }

    const size_t toRead = available < DOWNLOAD_CHUNK_SIZE ? available : DOWNLOAD_CHUNK_SIZE;
    const size_t bytesRead = stream->readBytes(buffer, toRead);

    if (bytesRead == 0) {
      break;
    }

    const size_t written = file.write(buffer, bytesRead);
    if (written != bytesRead) {
      Serial.printf("[%lu] [HTTP] Write failed: wrote %zu of %zu bytes\n", millis(), written, bytesRead);
      file.close();
      SdMan.remove(destPath.c_str());
      http.end();
      return FILE_ERROR;
    }

    downloaded += bytesRead;

    if (progress) {
      progress(downloaded, total);
    }
  }

  file.close();
  http.end();

  Serial.printf("[%lu] [HTTP] Downloaded %zu bytes\n", millis(), downloaded);

  // Verify download size if known
  if (contentLength > 0 && downloaded != contentLength) {
    Serial.printf("[%lu] [HTTP] Size mismatch: got %zu, expected %zu\n", millis(), downloaded, contentLength);
    SdMan.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFileNoAuth(const std::string& url,
                                                                   const std::string& destPath,
                                                                   ProgressCallback progress,
                                                                   const char* userAgent) {
  std::unique_ptr<WiFiClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new WiFiClientSecure();
    secureClient->setInsecure();
    client.reset(secureClient);
  } else {
    client.reset(new WiFiClient());
  }
  HTTPClient http;

  Serial.printf("[%lu] [HTTP] Download (no auth): %s\n", millis(), url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (userAgent != nullptr) {
    http.addHeader("User-Agent", userAgent);
  } else {
    http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  }
  http.setTimeout(60000);  // 60 s for EPUB downloads

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[%lu] [HTTP] Download failed: %d\n", millis(), httpCode);
    http.end();
    return HTTP_ERROR;
  }

  const size_t contentLength = http.getSize();

  if (SdMan.exists(destPath.c_str())) {
    SdMan.remove(destPath.c_str());
  }

  FsFile file;
  if (!SdMan.openFileForWrite("HTTP", destPath.c_str(), file)) {
    http.end();
    return FILE_ERROR;
  }

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    file.close();
    SdMan.remove(destPath.c_str());
    http.end();
    return HTTP_ERROR;
  }

  uint8_t buffer[DOWNLOAD_CHUNK_SIZE];
  size_t downloaded = 0;
  size_t lastYieldAt = 0;
  const size_t total = contentLength > 0 ? contentLength : 0;

  while (http.connected() && (contentLength == 0 || downloaded < contentLength)) {
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }
    const size_t toRead = available < DOWNLOAD_CHUNK_SIZE ? available : DOWNLOAD_CHUNK_SIZE;
    const size_t bytesRead = stream->readBytes(buffer, toRead);
    if (bytesRead == 0) break;
    const size_t written = file.write(buffer, bytesRead);
    if (written != bytesRead) {
      file.close();
      SdMan.remove(destPath.c_str());
      http.end();
      return FILE_ERROR;
    }
    downloaded += bytesRead;
    if (progress) {
      progress(downloaded, total);
      // Yield for UI only every 32KB to avoid slowing the loop with thousands of context switches
      if (downloaded - lastYieldAt >= DOWNLOAD_YIELD_EVERY_BYTES) {
        vTaskDelay(0);
        lastYieldAt = downloaded;
      }
    }
  }

  if (progress) {
    vTaskDelay(0);  // yield so UI can show 100% before we return
  }

  file.close();
  http.end();

  if (contentLength > 0 && downloaded != contentLength) {
    SdMan.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  return OK;
}
