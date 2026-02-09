#pragma once

/**
 * Try to connect to any nearby WiFi for which we have saved credentials.
 * Used when going to sleep so the device can be on a known network (e.g. for
 * next wake or for background sync). Runs a scan, picks the strongest
 * visible network we have creds for, and attempts connection with a short
 * timeout.
 * Safe to call from main loop / enterDeepSleep; may block for several seconds.
 */
namespace WifiAutoConnect {

void tryAutoConnectToKnownWifi();

}  // namespace WifiAutoConnect
