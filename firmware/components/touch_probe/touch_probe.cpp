#include "touch_probe.h"

#include <cstdio>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Bring-up diagnostic. Read-only apart from clearing the status register:
// it must never write the GT911 config block at 0x8047.
namespace touch_probe {
namespace {

constexpr const char* kTag = "touch_probe";
constexpr uint32_t kPollIntervalMs = 20;
constexpr int64_t kHeartbeatUs = 5 * 1000 * 1000;
constexpr uint32_t kTaskStackBytes = 4096;
constexpr uint16_t kRegConfig = 0x8047;
constexpr size_t kConfigLen = 186;  // 0x8047..0x8100: 184 config, checksum, fresh flag
constexpr uint16_t kRegProductId = 0x8140;
constexpr uint16_t kRegResolution = 0x8146;
constexpr uint16_t kRegStatus = 0x814E;
constexpr uint16_t kRegPoint0 = 0x8150;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;

esp_err_t ReadReg16(uint16_t reg, uint8_t* out, size_t len) {
    const uint8_t ptr[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF)};
    return i2c_master_transmit_receive(s_dev, ptr, sizeof(ptr), out, len, 50);
}

esp_err_t ClearStatus() {
    const uint8_t payload[3] = {static_cast<uint8_t>(kRegStatus >> 8),
                                static_cast<uint8_t>(kRegStatus & 0xFF), 0x00};
    return i2c_master_transmit(s_dev, payload, sizeof(payload), 50);
}

void ResetForAddress0x14() {
    gpio_config_t out_cfg = {};
    out_cfg.pin_bit_mask = (1ULL << BOARD_TP_RST_PIN) | (1ULL << BOARD_TP_INT_PIN);
    out_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out_cfg);

    gpio_set_level(BOARD_TP_RST_PIN, 0);
    gpio_set_level(BOARD_TP_INT_PIN, 1);  // high during the RST edge selects 0x14
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_TP_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_direction(BOARD_TP_INT_PIN, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(80));
}

void DumpIdentity() {
    uint8_t id[4] = {};
    esp_err_t err = ReadReg16(kRegProductId, id, sizeof(id));
    ESP_LOGI(kTag, "product id: %02X %02X %02X %02X (%s)", id[0], id[1], id[2], id[3],
             esp_err_to_name(err));

    uint8_t res[4] = {};
    err = ReadReg16(kRegResolution, res, sizeof(res));
    ESP_LOGI(kTag, "resolution reg: x=%u y=%u (%s)", res[0] | (res[1] << 8), res[2] | (res[3] << 8),
             esp_err_to_name(err));

    static uint8_t config[kConfigLen];
    err = ReadReg16(kRegConfig, config, sizeof(config));
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "config read failed: %s", esp_err_to_name(err));
        return;
    }
    uint8_t sum = 0;
    size_t nonzero = 0;
    for (size_t i = 0; i < kConfigLen - 1; ++i) {
        sum = static_cast<uint8_t>(sum + config[i]);
        nonzero += config[i] != 0;
    }
    ESP_LOGI(kTag, "config: version=0x%02X x=%u y=%u touches=%u nonzero=%u/185 sum=0x%02X fresh=0x%02X",
             config[0], config[1] | (config[2] << 8), config[3] | (config[4] << 8), config[5],
             static_cast<unsigned>(nonzero), sum, config[kConfigLen - 1]);
    for (size_t offset = 0; offset < kConfigLen; offset += 16) {
        char line[64] = {};
        size_t pos = 0;
        for (size_t i = offset; i < offset + 16 && i < kConfigLen; ++i) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", config[i]);
        }
        ESP_LOGI(kTag, "config[0x%02X]: %s", static_cast<unsigned>(offset), line);
    }
}

void ProbeTask(void*) {
    uint32_t polls = 0;
    uint32_t failures = 0;
    uint32_t ready_frames = 0;
    uint32_t touch_frames = 0;
    int last_status = -1;
    int last_int = -1;
    int64_t next_heartbeat = esp_timer_get_time() + kHeartbeatUs;

    while (true) {
        ++polls;
        const int int_level = gpio_get_level(BOARD_TP_INT_PIN);
        if (int_level != last_int) {
            ESP_LOGI(kTag, "INT changed: %d", int_level);
            last_int = int_level;
        }

        uint8_t status = 0;
        if (ReadReg16(kRegStatus, &status, 1) != ESP_OK) {
            ++failures;
        } else {
            if (status != last_status) {
                ESP_LOGI(kTag, "status changed: 0x%02X", status);
                last_status = status;
            }
            if (status & 0x80) {
                ++ready_frames;
                if ((status & 0x0F) > 0) {
                    ++touch_frames;
                    uint8_t point[8] = {};
                    if (ReadReg16(kRegPoint0, point, sizeof(point)) == ESP_OK) {
                        ESP_LOGI(kTag, "TOUCH points=%d x=%u y=%u size=%u", status & 0x0F,
                                 point[0] | (point[1] << 8), point[2] | (point[3] << 8),
                                 point[4] | (point[5] << 8));
                    }
                }
                ClearStatus();
            }
        }

        const int64_t now = esp_timer_get_time();
        if (now >= next_heartbeat) {
            ESP_LOGI(kTag, "heartbeat: polls=%lu failures=%lu ready=%lu touches=%lu INT=%d",
                     static_cast<unsigned long>(polls), static_cast<unsigned long>(failures),
                     static_cast<unsigned long>(ready_frames),
                     static_cast<unsigned long>(touch_frames), int_level);
            next_heartbeat = now + kHeartbeatUs;
        }
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

}  // namespace

esp_err_t Init() {
    gpio_config_t en_cfg = {};
    en_cfg.pin_bit_mask = 1ULL << BOARD_TP_PWR_EN_PIN;
    en_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&en_cfg);
    gpio_set_level(BOARD_TP_PWR_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(250));

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = BOARD_TP_I2C_SDA_PIN;
    bus_cfg.scl_io_num = BOARD_TP_I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return ESP_OK;  // a diagnostic must not stop the rest of the device booting
    }

    ResetForAddress0x14();

    err = i2c_master_probe(s_bus, BOARD_TP_I2C_ADDR, 200);
    ESP_LOGI(kTag, "probe 0x%02X: %s", BOARD_TP_I2C_ADDR, esp_err_to_name(err));
    if (err != ESP_OK) {
        return ESP_OK;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = BOARD_TP_I2C_ADDR;
    dev_cfg.scl_speed_hz = 100000;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "add_device failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    vTaskDelay(pdMS_TO_TICKS(30));
    DumpIdentity();
    ClearStatus();

    xTaskCreate(ProbeTask, "touch_probe", kTaskStackBytes, nullptr, 3, nullptr);
    ESP_LOGI(kTag, "polling every %lums; logging only on change",
             static_cast<unsigned long>(kPollIntervalMs));
    return ESP_OK;
}

}  // namespace touch_probe
