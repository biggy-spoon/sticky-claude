#include "button_service.h"

#include "board_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace button_service {
namespace {

constexpr const char* kTag = "button_service";
constexpr int kActiveLevel = 0;  // button pulls the pin low when pressed
constexpr uint32_t kPollIntervalMs = 15;
constexpr uint8_t kDebounceSamples = 3;  // consecutive matching reads to accept a transition
constexpr uint32_t kPollTaskStackWords = 2560;

PressStartHandler s_on_start = nullptr;
PressEndHandler s_on_end = nullptr;
void* s_context = nullptr;
TaskHandle_t s_task = nullptr;

void PollTask(void*) {
    bool pressed = false;
    uint8_t stable_count = 0;
    int last_raw = !kActiveLevel;
    int64_t press_started_us = 0;

    while (true) {
        const int raw = gpio_get_level(BOARD_BUTTON_AI_PIN);
        if (raw == last_raw) {
            if (stable_count < kDebounceSamples) {
                ++stable_count;
            }
        } else {
            last_raw = raw;
            stable_count = 1;
        }

        if (stable_count >= kDebounceSamples) {
            const bool is_active = (raw == kActiveLevel);
            if (is_active && !pressed) {
                pressed = true;
                press_started_us = esp_timer_get_time();
                ESP_LOGI(kTag, "press start");
                if (s_on_start != nullptr) {
                    s_on_start(s_context);
                }
            } else if (!is_active && pressed) {
                pressed = false;
                const uint32_t held_ms =
                    static_cast<uint32_t>((esp_timer_get_time() - press_started_us) / 1000);
                ESP_LOGI(kTag, "press end: held_ms=%lu", static_cast<unsigned long>(held_ms));
                if (s_on_end != nullptr) {
                    s_on_end(held_ms, s_context);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

}  // namespace

esp_err_t Init() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << BOARD_BUTTON_AI_PIN;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (s_task == nullptr) {
        const BaseType_t created =
            xTaskCreate(PollTask, "button_poll", kPollTaskStackWords, nullptr, 5, &s_task);
        if (created != pdPASS) {
            s_task = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(kTag, "initialized on GPIO%d", static_cast<int>(BOARD_BUTTON_AI_PIN));
    return ESP_OK;
}

void SetHandlers(PressStartHandler on_start, PressEndHandler on_end, void* context) {
    s_on_start = on_start;
    s_on_end = on_end;
    s_context = context;
}

}  // namespace button_service
