#include "wifi_service.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace wifi_service {
namespace {

constexpr const char* kTag = "wifi_service";
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr int kMaxRetries = 5;

constexpr int kConnectedBit = BIT0;
constexpr int kFailedBit = BIT1;

EventGroupHandle_t s_event_group = nullptr;
int s_retry_count = 0;
bool s_connected = false;
bool s_netif_initialized = false;

void EventHandler(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_count < kMaxRetries) {
            esp_wifi_connect();
            ++s_retry_count;
            ESP_LOGW(kTag, "disconnected, retry %d/%d", s_retry_count, kMaxRetries);
        } else {
            xEventGroupSetBits(s_event_group, kFailedBit);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        s_connected = true;
        xEventGroupSetBits(s_event_group, kConnectedBit);
    }
}

esp_err_t EnsureNetifInitialized() {
    if (s_netif_initialized) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_netif_initialized = true;
    return ESP_OK;
}

}  // namespace

esp_err_t ConnectStaBlocking(const std::string& ssid, const std::string& password) {
    esp_err_t err = EnsureNetifInitialized();
    if (err != ESP_OK) {
        return err;
    }

    if (s_event_group == nullptr) {
        s_event_group = xEventGroupCreate();
        if (s_event_group == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_event_group, kConnectedBit | kFailedBit);

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EventHandler,
                                               nullptr));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EventHandler, nullptr));

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid.c_str(),
                sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password.c_str(),
                sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    const EventBits_t bits = xEventGroupWaitBits(s_event_group, kConnectedBit | kFailedBit, pdFALSE,
                                                 pdFALSE, pdMS_TO_TICKS(kConnectTimeoutMs));

    if (bits & kConnectedBit) {
        ESP_LOGI(kTag, "connected");
        return ESP_OK;
    }
    ESP_LOGE(kTag, "failed to connect within %lums", static_cast<unsigned long>(kConnectTimeoutMs));
    return ESP_FAIL;
}

bool IsConnected() { return s_connected; }

esp_err_t StartAp(const std::string& ap_ssid, std::string* ap_ip_out) {
    esp_err_t err = EnsureNetifInitialized();
    if (err != ESP_OK) {
        return err;
    }

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    wifi_config_t ap_config = {};
    std::strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), ap_ssid.c_str(),
                sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = static_cast<uint8_t>(ap_ssid.size());
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (ap_ip_out != nullptr) {
        *ap_ip_out = "192.168.4.1";  // ESP-IDF's default AP netif address
    }
    ESP_LOGI(kTag, "AP started: ssid=%s", ap_ssid.c_str());
    return ESP_OK;
}

}  // namespace wifi_service
