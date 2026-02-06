#pragma once
#include <SDCardManager.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files.
 * Wraps WiFiClientSecure and HTTPClient for HTTPS requests.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Fetch text content from a URL.
   * @param url The URL to fetch
   * @param outContent The fetched content (output)
   * @return true if fetch succeeded, false on error
   */
  static bool fetchUrl(const std::string& url, std::string& outContent);

  static bool fetchUrl(const std::string& url, Stream& stream);

  /**
   * Fetch from URL without adding OPDS Basic auth (e.g. for Kobo/BookLore token-in-URL APIs).
   * @param userAgent If non-null, use this User-Agent (e.g. "Kobo eReader" to avoid 403 from Kobo APIs).
   */
  static bool fetchUrlNoAuth(const std::string& url, std::string& outContent, const char* userAgent = nullptr);

  /**
   * Kobo library/sync token-based pagination: send X-Kobo-SyncToken (if non-empty), return body and
   * response headers X-Kobo-SyncToken and X-Kobo-Sync (e.g. "continue" when more pages exist).
   * Do not use ?page= or ?pageSize=; the server ignores them and uses the token.
   */
  struct KoboSyncHeaders {
    std::string requestSyncToken;   // send as X-Kobo-SyncToken if non-empty
    std::string responseSyncToken;  // filled from response X-Kobo-SyncToken
    std::string responseSync;       // filled from response X-Kobo-Sync ("continue" = more pages)
  };
  static bool fetchUrlNoAuthKoboSync(const std::string& url, std::string& outContent,
                                     const char* userAgent, KoboSyncHeaders* koboHeaders);

  /**
   * Download to file without adding OPDS Basic auth.
   * @param userAgent If non-null, use this User-Agent (e.g. "Kobo eReader" for Kobo APIs).
   */
  static DownloadError downloadToFileNoAuth(const std::string& url, const std::string& destPath,
                                            ProgressCallback progress = nullptr,
                                            const char* userAgent = nullptr);

  /**
   * Download a file to the SD card.
   * @param url The URL to download
   * @param destPath The destination path on SD card
   * @param progress Optional progress callback
   * @return DownloadError indicating success or failure type
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr);

 private:
  // 4KB chunks: balance throughput vs stack (loopTask overflows with 8KB; upload uses 4KB)
  static constexpr size_t DOWNLOAD_CHUNK_SIZE = 4096;
  static constexpr size_t DOWNLOAD_YIELD_EVERY_BYTES = 4096;  // yield every 4KB so progress bar updates
};
