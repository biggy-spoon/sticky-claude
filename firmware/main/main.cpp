#include <cstdlib>

#include "assistant_service.h"
#include "button_service.h"
#include "captive_portal.h"
#include "device_nav.h"
#include "epaper_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "recording_service.h"
#include "settings_service.h"
#include "ui.h"
#include "wifi_service.h"

namespace {

constexpr const char* kTag = "main";
constexpr uint32_t kMinHoldMs = 250;  // ignore accidental taps shorter than this
constexpr uint32_t kAssistantTaskStackWords = 6144;

TaskHandle_t s_assistant_task = nullptr;
bool s_wifi_connected = false;

void OnPressStart(button_service::ButtonId id, void*) {
    if (id != button_service::ButtonId::kAi) {
        return;  // Up/Down only act on release -- see OnPressEnd
    }
    // Started optimistically; a press short enough to turn out to be a tap
    // (handled below) just discards this via StopCapture(), same as
    // before Up/Down existed.
    ESP_LOGI(kTag, "recording...");
    recording_service::StartCapture();
    ui::ShowRecording();
}

void OnPressEnd(button_service::ButtonId id, uint32_t held_ms, void*) {
    if (id == button_service::ButtonId::kUp) {
        device_nav::OnUp();
        return;
    }
    if (id == button_service::ButtonId::kDown) {
        device_nav::OnDown();
        return;
    }

    size_t sample_count = 0;
    recording_service::StopCapture(&sample_count);

    if (held_ms < kMinHoldMs || sample_count == 0) {
        // Too short to be an intentional recording -- treat it as a tap:
        // confirm/toggle whatever's highlighted if a device view is
        // showing, otherwise just go back to idle like before this
        // button did double duty.
        ESP_LOGI(kTag, "press too short (%lums) or no audio, treating as a tap",
                 static_cast<unsigned long>(held_ms));
        if (!device_nav::OnConfirm()) {
            ui::ShowIdle(s_wifi_connected);
        }
        return;
    }

    device_nav::ResetToHome();
    xTaskNotifyGive(s_assistant_task);
}

void AssistantTask(void*) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint8_t* wav = nullptr;
        size_t wav_len = 0;
        if (recording_service::BuildWav(&wav, &wav_len) != ESP_OK) {
            ESP_LOGW(kTag, "BuildWav failed");
            continue;
        }

        ESP_LOGI(kTag, "sending %u byte clip to backend", static_cast<unsigned>(wav_len));
        ui::ShowThinking();

        const assistant_service::ConverseResult result = assistant_service::Converse(wav, wav_len);
        free(wav);

        if (result.success) {
            ESP_LOGI(kTag, "transcript=%s", result.transcript.c_str());
            ESP_LOGI(kTag, "reply=%s", result.reply.c_str());
            ui::ShowReply(result.transcript, result.reply);
        } else {
            ESP_LOGW(kTag, "converse failed: http=%d error=%s", result.http_status,
                     result.error_message.c_str());
            ui::ShowError(result.error_message.empty() ? "Unknown error" : result.error_message);
        }
    }
}

}  // namespace

extern "C" void app_main() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(settings_service::Init());

    if (!settings_service::IsConfigured()) {
        ESP_LOGI(kTag, "not configured yet, starting setup portal");
        ESP_ERROR_CHECK(captive_portal::Run());
        // Run() only returns on failure to start; the portal itself reboots
        // the device once setup completes. Nothing else to do here.
        return;
    }

    ESP_ERROR_CHECK(epaper_display::Init());
    ESP_ERROR_CHECK(ui::Init());
    ESP_ERROR_CHECK(recording_service::Init());
    ESP_ERROR_CHECK(button_service::Init());
    ESP_ERROR_CHECK(device_nav::Init());

    xTaskCreate(AssistantTask, "assistant", kAssistantTaskStackWords, nullptr, 5,
                &s_assistant_task);

    button_service::SetHandlers(&OnPressStart, &OnPressEnd, nullptr);

    const settings_service::Settings settings = settings_service::Get();
    ESP_LOGI(kTag, "connecting to WiFi...");
    s_wifi_connected =
        wifi_service::ConnectStaBlocking(settings.wifi_ssid, settings.wifi_password) == ESP_OK;
    if (!s_wifi_connected) {
        ESP_LOGE(kTag, "WiFi connect failed; recording will work but uploads will fail");
    }

    ui::ShowIdle(s_wifi_connected);
    ESP_LOGI(kTag, "ready: hold the AI button to record, Up/Down to page through plugin views");
}
