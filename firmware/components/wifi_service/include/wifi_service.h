#pragma once

#include <string>

#include "esp_err.h"

// WiFi station connect (using runtime-supplied credentials, not Kconfig) and
// a SoftAP mode used by the captive portal during first-run setup.
namespace wifi_service {

// Blocks until connected (DHCP lease obtained) or kConnectTimeoutMs elapses.
esp_err_t ConnectStaBlocking(const std::string& ssid, const std::string& password);

bool IsConnected();

// Starts an open SoftAP with the given SSID (used only for the setup
// portal). ap_ip_out receives the AP's own IP (always 192.168.4.1 with
// ESP-IDF's default AP netif config) for the captive portal's DNS/HTTP
// server to bind to.
esp_err_t StartAp(const std::string& ap_ssid, std::string* ap_ip_out);

}  // namespace wifi_service
