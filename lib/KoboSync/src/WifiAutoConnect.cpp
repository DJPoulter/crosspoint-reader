#include "WifiAutoConnect.h"

#include <WiFi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "WifiCredentialStore.h"

namespace {
constexpr unsigned long CONNECT_TIMEOUT_MS = 12000;  // AP may be slow to respond when device was disconnected
}  // namespace

namespace WifiAutoConnect {

void tryAutoConnectToKnownWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    return;  // Already connected
  }

  WIFI_STORE.loadFromFile();
  if (WIFI_STORE.getCredentials().empty()) {
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  const int n = WiFi.scanNetworks();  // synchronous scan
  if (n <= 0) {
    if (n == WIFI_SCAN_FAILED) {
      Serial.printf("[%lu] [WIFI] Auto-connect: scan failed\n", millis());
    }
    WiFi.scanDelete();
    return;
  }

  struct NetworkWithRssi {
    std::string ssid;
    int32_t rssi;
  };
  std::vector<NetworkWithRssi> known;
  known.reserve(static_cast<size_t>(n));

  for (int i = 0; i < n; i++) {
    const std::string ssid = WiFi.SSID(i).c_str();
    if (ssid.empty()) continue;
    if (!WIFI_STORE.hasSavedCredential(ssid)) continue;
    const int32_t rssi = WiFi.RSSI(i);
    known.push_back({ssid, rssi});
  }
  WiFi.scanDelete();

  if (known.empty()) {
    return;
  }

  std::sort(known.begin(), known.end(),
            [](const NetworkWithRssi& a, const NetworkWithRssi& b) { return a.rssi > b.rssi; });

  for (const auto& net : known) {
    const WifiCredential* cred = WIFI_STORE.findCredential(net.ssid);
    if (!cred || cred->password.empty()) continue;

    Serial.printf("[%lu] [WIFI] Auto-connect: trying %s (RSSI %ld)\n", millis(), net.ssid.c_str(),
                  static_cast<long>(net.rssi));

    WiFi.begin(net.ssid.c_str(), cred->password.c_str());
    const unsigned long start = millis();
    while (millis() - start < CONNECT_TIMEOUT_MS) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[%lu] [WIFI] Auto-connect: connected to %s\n", millis(), net.ssid.c_str());
        return;
      }
      delay(100);
    }
    Serial.printf("[%lu] [WIFI] Auto-connect: timeout for %s\n", millis(), net.ssid.c_str());
    WiFi.disconnect();
    delay(100);
  }
  Serial.printf("[%lu] [WIFI] Auto-connect: no network connected\n", millis());
}

}  // namespace WifiAutoConnect
