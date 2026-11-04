#include "settings_service.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace settings_service {
namespace {

constexpr const char* kTag = "settings_service";
constexpr const char* kNamespace = "sticky";
constexpr const char* kKeySsid = "wifi_ssid";
constexpr const char* kKeyPassword = "wifi_pass";
constexpr const char* kKeyBackendUrl = "backend_url";

std::string ReadString(nvs_handle_t handle, const char* key) {
    size_t size = 0;
    if (nvs_get_str(handle, key, nullptr, &size) != ESP_OK || size == 0) {
        return {};
    }
    std::string value(size, '\0');
    if (nvs_get_str(handle, key, value.data(), &size) != ESP_OK) {
        return {};
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

}  // namespace

esp_err_t Init() {
    // main.cpp already calls nvs_flash_init() for the whole app; nothing
    // extra to set up here.
    return ESP_OK;
}

bool IsConfigured() { return !Get().wifi_ssid.empty(); }

Settings Get() {
    Settings settings = {};
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return settings;
    }
    settings.wifi_ssid = ReadString(handle, kKeySsid);
    settings.wifi_password = ReadString(handle, kKeyPassword);
    settings.backend_url = ReadString(handle, kKeyBackendUrl);
    nvs_close(handle);
    return settings;
}

esp_err_t Save(const Settings& settings) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, kKeySsid, settings.wifi_ssid.c_str());
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kKeyPassword, settings.wifi_password.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kKeyBackendUrl, settings.backend_url.c_str());
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t ClearAll() {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

}  // namespace settings_service
