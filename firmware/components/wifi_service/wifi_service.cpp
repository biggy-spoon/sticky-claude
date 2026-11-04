#include "wifi_service.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

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

}  // namespace

esp_err_t ConnectBlocking() {
    s_event_group = xEventGroupCreate();
    if (s_event_group == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EventHandler,
                                               nullptr));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EventHandler, nullptr));

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), CONFIG_STICKY_CLAUDE_WIFI_SSID,
                sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
                CONFIG_STICKY_CLAUDE_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
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

}  // namespace wifi_service
