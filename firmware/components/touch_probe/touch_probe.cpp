#include "touch_probe.h"

#include <cstdio>
#include <cstring>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Reset/bring-up sequence matches the one shared by every independent
// community GT911 driver found for this exact device (InkSalah, the 2048
// game, Sticky Bunny, the handwriting pad -- all github.com/limengdu or
// similarly named reTerminal-Sticky projects): none of them write a GT911
// config block (register 0x8047) at all, so that's dropped here too. The
// first attempt at this probe wrote one anyway, guessing the chip needed
// it; it didn't help, and six independent confirmed-working
// implementations agree it's unnecessary.
namespace touch_probe {
namespace {

constexpr const char* kTag = "touch_probe";
constexpr uint32_t kPollIntervalMs = 500;
constexpr size_t kDumpBytes = 16;
constexpr uint32_t kTaskStackWords = 3072;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;
uint16_t s_dev_addr = BOARD_TP_I2C_ADDR;

void ConfigureOutput(gpio_num_t pin, int level) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&cfg);
    gpio_set_level(pin, level);
}

void HexDump(const char* label, const uint8_t* buf, size_t len) {
    char hex[kDumpBytes * 3 + 1] = {};
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 3 < sizeof(hex); ++i) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
    }
    ESP_LOGI(kTag, "%s: %s", label, hex);
}

// GT911 registers are addressed with a 16-bit, big-endian pointer.
esp_err_t ReadReg16(uint16_t reg, uint8_t* out, size_t len) {
    const uint8_t ptr[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF)};
    return i2c_master_transmit_receive(s_dev, ptr, sizeof(ptr), out, len, 200);
}

esp_err_t WriteReg16(uint16_t reg, uint8_t value) {
    const uint8_t payload[3] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF),
                                value};
    return i2c_master_transmit(s_dev, payload, sizeof(payload), 200);
}

// Resets the chip while actively driving INT (not just leaving it pulled
// up) at the level that selects `for_address` -- per the GT911 datasheet
// and every reference driver found for this device, INT's level *during*
// the reset pulse is what picks which I2C address the chip answers on
// afterward (0x5D for low, 0x14 for high). Timing (20ms low, 20ms high,
// 80ms settle) matches those drivers exactly.
void ResetForAddress(int int_level_for_address) {
    gpio_config_t reset_out_cfg = {};
    reset_out_cfg.pin_bit_mask = (1ULL << BOARD_TP_RST_PIN) | (1ULL << BOARD_TP_INT_PIN);
    reset_out_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&reset_out_cfg);

    gpio_set_level(BOARD_TP_RST_PIN, 0);
    gpio_set_level(BOARD_TP_INT_PIN, int_level_for_address);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_TP_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    gpio_config_t int_in_cfg = {};
    int_in_cfg.pin_bit_mask = 1ULL << BOARD_TP_INT_PIN;
    int_in_cfg.mode = GPIO_MODE_INPUT;  // release INT back to its normal interrupt-input role
    gpio_config(&int_in_cfg);
    vTaskDelay(pdMS_TO_TICKS(80));
}

void ReadIdAndResolution() {
    uint8_t product_id[4] = {};
    esp_err_t err = ReadReg16(0x8140, product_id, sizeof(product_id));
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "GT911 product id bytes: %02X %02X %02X %02X ('%c%c%c%c')",
                 product_id[0], product_id[1], product_id[2], product_id[3],
                 product_id[0] ? product_id[0] : '.', product_id[1] ? product_id[1] : '.',
                 product_id[2] ? product_id[2] : '.', product_id[3] ? product_id[3] : '.');
    } else {
        ESP_LOGW(kTag, "product id read failed: %s", esp_err_to_name(err));
    }

    uint8_t res[4] = {};
    err = ReadReg16(0x8146, res, sizeof(res));
    if (err == ESP_OK) {
        const uint16_t x_res = res[0] | (static_cast<uint16_t>(res[1]) << 8);
        const uint16_t y_res = res[2] | (static_cast<uint16_t>(res[3]) << 8);
        ESP_LOGI(kTag, "sensor native resolution: x=%u y=%u", x_res, y_res);
    } else {
        ESP_LOGW(kTag, "resolution read failed: %s", esp_err_to_name(err));
    }
}

// One status+point0 read/log cycle, clearing the ready flag if set.
// Shared by the pre-WiFi blocking test in Init() and the ongoing
// ProbeTask poll loop, so both report identically.
void PollOnce() {
    const int int_level = gpio_get_level(BOARD_TP_INT_PIN);

    uint8_t status = 0;
    esp_err_t err = ReadReg16(0x814E, &status, 1);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "INT=%d status read failed: %s", int_level, esp_err_to_name(err));
        return;
    }

    uint8_t buf[kDumpBytes] = {};
    err = ReadReg16(0x8150, buf, 8);
    ESP_LOGI(kTag, "INT=%d status=0x%02X (ready=%d points=%d)", int_level, status,
             (status & 0x80) ? 1 : 0, status & 0x0F);
    if (err == ESP_OK) {
        HexDump("  point0", buf, 8);
    } else {
        ESP_LOGW(kTag, "  point0 read failed: %s", esp_err_to_name(err));
    }

    if (status & 0x80) {
        WriteReg16(0x814E, 0x00);
    }
}

void ProbeTask(void*) {
    while (true) {
        PollOnce();
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
}

}  // namespace

esp_err_t Init() {
    // Diagnostic-only delay: gives a serial monitor time to (re)connect
    // before the one-time probe/scan output below, which otherwise tends
    // to race a fresh serial connection and get lost. Remove once the
    // protocol is known and this becomes a real driver.
    ESP_LOGI(kTag, "waiting 5s before probing (connect a monitor now)...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    // Confirmed active-high: driving it low kills I2C entirely. Reference
    // drivers wait a full 250ms after enabling power before doing
    // anything else -- noticeably longer than this file's first attempt
    // (20ms), which may have started the reset pulse before the chip's
    // power rail had actually stabilized.
    ConfigureOutput(BOARD_TP_PWR_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(250));

    ResetForAddress(1);  // 1 -> address 0x14

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = -1;
    bus_cfg.sda_io_num = BOARD_TP_I2C_SDA_PIN;
    bus_cfg.scl_io_num = BOARD_TP_I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    // Reference drivers probe 0x14 first, then fall back to 0x5D -- match
    // that instead of assuming 0x14 is the only possibility.
    constexpr uint16_t kAltAddr = 0x5D;
    err = i2c_master_probe(s_bus, BOARD_TP_I2C_ADDR, 200);
    if (err == ESP_OK) {
        s_dev_addr = BOARD_TP_I2C_ADDR;
    } else {
        ESP_LOGW(kTag, "probe 0x%02X failed (%s); trying 0x%02X", BOARD_TP_I2C_ADDR,
                 esp_err_to_name(err), kAltAddr);
        err = i2c_master_probe(s_bus, kAltAddr, 200);
        s_dev_addr = kAltAddr;
    }
    ESP_LOGI(kTag, "probe 0x%02X: %s", s_dev_addr,
             err == ESP_OK ? "ACK (device present)" : esp_err_to_name(err));

    // Also scan the full 7-bit range once, in case the touch IC answers at
    // a different address than expected.
    for (uint16_t addr = 0x08; addr <= 0x77; ++addr) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(kTag, "bus scan: device present at 0x%02X", addr);
        }
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = s_dev_addr;
    dev_cfg.scl_speed_hz = 100000;
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    ReadIdAndResolution();
    vTaskDelay(pdMS_TO_TICKS(30));
    WriteReg16(0x814E, 0x00);

    // Blocks main.cpp's startup here -- before WiFi connects at all --
    // for a dedicated touch test window. Every prior test ran with WiFi
    // already joined (it connects ~2-3s into boot, well before any
    // manual test so far); ESP32 WiFi radio activity is a known source
    // of power-rail noise, and capacitive sensing is analog/noise-
    // sensitive in a way I2C's digital signaling isn't -- which would
    // explain perfect I2C communication alongside zero sensing activity
    // exactly as seen. This isolates that variable directly.
    ESP_LOGI(kTag, "=== PRE-WIFI TOUCH TEST: touch the screen now, WiFi is not yet connected ===");
    for (int i = 0; i < 30; ++i) {
        PollOnce();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(kTag, "=== PRE-WIFI TOUCH TEST DONE -- continuing boot (WiFi connects next) ===");

    TaskHandle_t task = nullptr;
    const BaseType_t created =
        xTaskCreate(ProbeTask, "touch_probe", kTaskStackWords, nullptr, 3, &task);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "initialized: SCL=GPIO%d SDA=GPIO%d INT=GPIO%d addr=0x%02X",
             static_cast<int>(BOARD_TP_I2C_SCL_PIN), static_cast<int>(BOARD_TP_I2C_SDA_PIN),
             static_cast<int>(BOARD_TP_INT_PIN), s_dev_addr);
    return ESP_OK;
}

}  // namespace touch_probe
