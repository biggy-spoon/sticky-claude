#pragma once

#include <string>

#include "esp_err.h"

// Runtime-configurable settings (WiFi credentials, backend URL), persisted
// in NVS. Replaces the old Kconfig-only setup so the captive portal can
// write these without a recompile.
namespace settings_service {

struct Settings {
    std::string wifi_ssid;
    std::string wifi_password;
    std::string backend_url;
};

esp_err_t Init();

// True once a WiFi SSID has been saved (the signal that first-run setup is
// done and main.cpp should connect normally instead of starting the portal).
bool IsConfigured();

Settings Get();
esp_err_t Save(const Settings& settings);
esp_err_t ClearAll();

}  // namespace settings_service
