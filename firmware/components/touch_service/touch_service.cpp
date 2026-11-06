#include "touch_service.h"

#include <cstdlib>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace touch_service {
namespace {

constexpr const char* kTag = "touch_service";
constexpr uint32_t kTaskStackBytes = 4096;
constexpr uint32_t kPollIntervalMs = 20;
constexpr uint32_t kI2cHz = 100000;
constexpr int kI2cTimeoutMs = 50;
constexpr int kStartAttempts = 3;
constexpr int kFailuresBeforeRestart = 50;

constexpr uint16_t kRegProductId = 0x8140;
constexpr uint16_t kRegResolution = 0x8146;
constexpr uint16_t kRegStatus = 0x814E;
constexpr uint16_t kRegPoint0 = 0x8150;
constexpr uint8_t kStatusReady = 0x80;
constexpr uint8_t kStatusCountMask = 0x0F;

constexpr int64_t kMaxTapUs = 800 * 1000;
constexpr int kMaxTapMovePx = 24;
constexpr int64_t kMaxSwipeUs = 1500 * 1000;
constexpr int kMinSwipePx = 60;
// Ends a contact if the chip stops sending frames without a release frame.
constexpr int64_t kContactStaleUs = 150 * 1000;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;
GestureHandler s_handler = nullptr;
void* s_context = nullptr;

esp_err_t ReadReg(uint16_t reg, uint8_t* out, size_t len) {
    const uint8_t ptr[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF)};
    return i2c_master_transmit_receive(s_dev, ptr, sizeof(ptr), out, len, kI2cTimeoutMs);
}

esp_err_t ClearStatus() {
    const uint8_t payload[3] = {static_cast<uint8_t>(kRegStatus >> 8),
                                static_cast<uint8_t>(kRegStatus & 0xFF), 0x00};
    return i2c_master_transmit(s_dev, payload, sizeof(payload), kI2cTimeoutMs);
}

// Measured on hardware: the panel's sensor is portrait 480x800, with both axes
// reversed relative to the landscape display.
void MapToScreen(uint16_t raw_x, uint16_t raw_y, int* x, int* y) {
    *x = BOARD_EPD_WIDTH - 1 - raw_y;
    *y = BOARD_EPD_HEIGHT - 1 - raw_x;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

// On this board the chip only finishes starting with a real RST pulse followed
// by INT held low for 50ms. Without that step it still answers I2C and returns
// its product ID, but its config and resolution registers read as zero and it
// never reports a touch.
void PowerCycleAndReset() {
    gpio_set_direction(BOARD_TP_RST_PIN, GPIO_MODE_INPUT);
    gpio_set_direction(BOARD_TP_INT_PIN, GPIO_MODE_INPUT);
    gpio_set_level(BOARD_TP_PWR_EN_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(BOARD_TP_PWR_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(250));

    gpio_config_t out_cfg = {};
    out_cfg.pin_bit_mask = (1ULL << BOARD_TP_RST_PIN) | (1ULL << BOARD_TP_INT_PIN);
    out_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out_cfg);

    gpio_set_level(BOARD_TP_RST_PIN, 0);
    gpio_set_level(BOARD_TP_INT_PIN, 1);  // high across the RST edge selects address 0x14
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_TP_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_TP_INT_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_direction(BOARD_TP_INT_PIN, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(110));
}

bool ChipReady() {
    uint8_t id[4] = {};
    uint8_t res[4] = {};
    if (ReadReg(kRegProductId, id, sizeof(id)) != ESP_OK ||
        ReadReg(kRegResolution, res, sizeof(res)) != ESP_OK) {
        return false;
    }
    const unsigned x = res[0] | (res[1] << 8);
    const unsigned y = res[2] | (res[3] << 8);
    ESP_LOGI(kTag, "chip id=%c%c%c sensor=%ux%u", id[0], id[1], id[2], x, y);
    return x != 0 && y != 0;
}

bool StartChip() {
    for (int attempt = 0; attempt < kStartAttempts; ++attempt) {
        PowerCycleAndReset();
        if (ChipReady()) {
            ClearStatus();
            return true;
        }
    }
    return false;
}

void TouchTask(void*) {
    bool touching = false;
    bool moved = false;
    int start_x = 0;
    int start_y = 0;
    int last_x = 0;
    int last_y = 0;
    int64_t start_us = 0;
    int64_t last_frame_us = 0;
    int failures = 0;

    auto end_contact = [&](int64_t now) {
        touching = false;
        if (s_handler == nullptr) {
            return;
        }
        const int64_t duration = now - start_us;
        const int dx = last_x - start_x;
        const int dy = last_y - start_y;
        Gesture gesture = {GestureType::kTap, start_x, start_y};
        if (!moved) {
            if (duration > kMaxTapUs) {
                return;
            }
        } else if (duration > kMaxSwipeUs || (abs(dx) < kMinSwipePx && abs(dy) < kMinSwipePx)) {
            return;
        } else if (abs(dx) > abs(dy)) {
            gesture.type = dx < 0 ? GestureType::kSwipeLeft : GestureType::kSwipeRight;
        } else {
            gesture.type = dy < 0 ? GestureType::kSwipeUp : GestureType::kSwipeDown;
        }
        ESP_LOGI(kTag, "gesture type=%d x=%d y=%d dx=%d dy=%d", static_cast<int>(gesture.type),
                 start_x, start_y, dx, dy);
        s_handler(gesture, s_context);
    };

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
        const int64_t now = esp_timer_get_time();

        uint8_t status = 0;
        if (ReadReg(kRegStatus, &status, 1) != ESP_OK) {
            if (++failures >= kFailuresBeforeRestart) {
                ESP_LOGW(kTag, "%d read failures, restarting the chip", failures);
                touching = false;
                failures = 0;
                if (!StartChip()) {
                    ESP_LOGE(kTag, "chip restart failed");
                }
            }
            continue;
        }
        failures = 0;

        if ((status & kStatusReady) == 0) {
            if (touching && now - last_frame_us > kContactStaleUs) {
                end_contact(now);
            }
            continue;
        }

        if ((status & kStatusCountMask) == 0) {
            if (touching) {
                end_contact(now);
            }
        } else {
            uint8_t point[8] = {};
            if (ReadReg(kRegPoint0, point, sizeof(point)) == ESP_OK) {
                int x = 0;
                int y = 0;
                MapToScreen(point[0] | (point[1] << 8), point[2] | (point[3] << 8), &x, &y);
                last_frame_us = now;
                last_x = x;
                last_y = y;
                if (!touching) {
                    touching = true;
                    moved = false;
                    start_x = x;
                    start_y = y;
                    start_us = now;
                } else if (abs(x - start_x) > kMaxTapMovePx || abs(y - start_y) > kMaxTapMovePx) {
                    moved = true;
                }
            }
        }
        ClearStatus();
    }
}

}  // namespace

void SetGestureHandler(GestureHandler handler, void* context) {
    s_handler = handler;
    s_context = context;
}

esp_err_t Init() {
    gpio_config_t en_cfg = {};
    en_cfg.pin_bit_mask = 1ULL << BOARD_TP_PWR_EN_PIN;
    en_cfg.mode = GPIO_MODE_OUTPUT;
    esp_err_t err = gpio_config(&en_cfg);
    if (err != ESP_OK) {
        return err;
    }

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = BOARD_TP_I2C_SDA_PIN;
    bus_cfg.scl_io_num = BOARD_TP_I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = BOARD_TP_I2C_ADDR;
    dev_cfg.scl_speed_hz = kI2cHz;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    if (!StartChip()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (xTaskCreate(TouchTask, "touch", kTaskStackBytes, nullptr, 4, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "ready");
    return ESP_OK;
}

}  // namespace touch_service
