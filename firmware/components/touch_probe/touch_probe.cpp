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

enum class Variant {
    kDirectionOnly,   // reference driver: gpio_set_direction, RST pad mux left at its default
    kGpioConfig,      // RST really driven via gpio_config, no INT sync
    kGpioConfigSync,  // as above, plus the datasheet's 50ms INT-low sync
    kNoReset,         // power cycle only, RST never driven
};

const char* VariantName(Variant v) {
    switch (v) {
        case Variant::kDirectionOnly: return "A direction-only";
        case Variant::kGpioConfig: return "B gpio_config";
        case Variant::kGpioConfigSync: return "C gpio_config+int-sync";
        case Variant::kNoReset: return "D no-reset";
    }
    return "?";
}

void PowerCycle() {
    gpio_set_direction(BOARD_TP_RST_PIN, GPIO_MODE_INPUT);
    gpio_set_direction(BOARD_TP_INT_PIN, GPIO_MODE_INPUT);
    gpio_set_level(BOARD_TP_PWR_EN_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(BOARD_TP_PWR_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(250));
}

void ApplyVariant(Variant v) {
    if (v == Variant::kNoReset) {
        vTaskDelay(pdMS_TO_TICKS(120));
        return;
    }
    if (v == Variant::kDirectionOnly) {
        gpio_set_direction(BOARD_TP_RST_PIN, GPIO_MODE_OUTPUT);
        gpio_set_direction(BOARD_TP_INT_PIN, GPIO_MODE_OUTPUT);
    } else {
        gpio_config_t out_cfg = {};
        out_cfg.pin_bit_mask = (1ULL << BOARD_TP_RST_PIN) | (1ULL << BOARD_TP_INT_PIN);
        out_cfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&out_cfg);
    }
    gpio_set_level(BOARD_TP_RST_PIN, 0);
    gpio_set_level(BOARD_TP_INT_PIN, 1);  // high during the RST edge selects 0x14
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_TP_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (v == Variant::kGpioConfigSync) {
        gpio_set_level(BOARD_TP_INT_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    gpio_set_direction(BOARD_TP_INT_PIN, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(80));
}

void DetachDevice() {
    if (s_dev != nullptr) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = nullptr;
    }
}

bool AttachDevice(uint16_t addr) {
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) == ESP_OK;
}

// Valid means the chip reports a real resolution, which the reference firmware
// gets on this same board and our earlier bring-up never did.
bool TryVariant(Variant v) {
    DetachDevice();
    PowerCycle();
    ApplyVariant(v);
    vTaskDelay(pdMS_TO_TICKS(30));

    const uint16_t addrs[] = {0x14, 0x5D};
    for (uint16_t addr : addrs) {
        if (i2c_master_probe(s_bus, addr, 50) != ESP_OK || !AttachDevice(addr)) {
            continue;
        }
        uint8_t id[4] = {};
        uint8_t res[4] = {};
        const esp_err_t id_err = ReadReg16(kRegProductId, id, sizeof(id));
        const esp_err_t res_err = ReadReg16(kRegResolution, res, sizeof(res));
        const unsigned x = res[0] | (res[1] << 8);
        const unsigned y = res[2] | (res[3] << 8);
        const bool valid = id_err == ESP_OK && res_err == ESP_OK && x != 0 && y != 0;
        ESP_LOGI(kTag, "VARIANT %s: addr=0x%02X id=%02X%02X%02X x=%u y=%u -> %s", VariantName(v),
                 addr, id[0], id[1], id[2], x, y, valid ? "VALID" : "invalid");
        return valid;
    }
    ESP_LOGI(kTag, "VARIANT %s: no device on 0x14 or 0x5D", VariantName(v));
    return false;
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

    // Direction-only runs first: gpio_config in the later variants switches
    // the RST pad to GPIO mode, which can't be undone without a reboot here.
    const Variant variants[] = {Variant::kDirectionOnly, Variant::kNoReset, Variant::kGpioConfig,
                                Variant::kGpioConfigSync};
    bool have_winner = false;
    Variant winner = Variant::kNoReset;
    for (Variant v : variants) {
        if (TryVariant(v) && !have_winner) {
            have_winner = true;
            winner = v;
        }
    }
    ESP_LOGI(kTag, "VARIANT RESULT: %s", have_winner ? VariantName(winner) : "none valid");

    // Leave the chip in a state worth polling. Variant A can't be replayed after
    // the RST pad changed mode, so D stands in for it: both skip a real reset
    // if the mux theory holds.
    const Variant final_variant = !have_winner || winner == Variant::kDirectionOnly
                                      ? Variant::kNoReset
                                      : winner;
    TryVariant(final_variant);
    if (s_dev == nullptr) {
        ESP_LOGW(kTag, "no device attached; not polling");
        return ESP_OK;
    }
    DumpIdentity();
    ClearStatus();

    xTaskCreate(ProbeTask, "touch_probe", kTaskStackBytes, nullptr, 3, nullptr);
    ESP_LOGI(kTag, "polling every %lums; logging only on change",
             static_cast<unsigned long>(kPollIntervalMs));
    return ESP_OK;
}

}  // namespace touch_probe
