#pragma once

#include "esp_err.h"

// Minimal blocking WiFi station connect. SSID/password come from Kconfig
// (idf.py menuconfig -> "sticky-claude WiFi") so credentials don't end up
// hardcoded in a source file that gets committed.
namespace wifi_service {

// Blocks until connected (DHCP lease obtained) or kConnectTimeoutMs elapses.
esp_err_t ConnectBlocking();

bool IsConnected();

}  // namespace wifi_service
