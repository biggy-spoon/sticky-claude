// The SSD1677 init/refresh sequence below is ported from the folloup-sticky
// project's ssd1677_driver.cpp (https://github.com/alxv2016/folloup-sticky),
// which is confirmed working on this exact board/panel. That project is
// GPLv3-licensed; per its terms, this file -- as a derivative work -- is
// also licensed under the GNU GPLv3 (see LICENSE in this directory), unlike
// the rest of this repository (MIT). The original author's copyright is
// retained in spirit: this is a port, not independent work.
#include "epaper_display.h"

#include <algorithm>
#include <cstring>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace epaper_display {
namespace {

constexpr const char* kTag = "epaper_display";
constexpr uint32_t kSpiClockHz = 4 * 1000 * 1000;
constexpr uint32_t kBusyPollIntervalMs = 20;
constexpr uint32_t kBusyTimeoutMs = 20000;

// SSD1677 command opcodes, and the register values below, ported from the
// proven folloup-sticky driver (see file header).
constexpr uint8_t kCmdBoosterSoftStart = 0x0C;
constexpr uint8_t kCmdDriverOutputControl = 0x01;
constexpr uint8_t kCmdDataEntryMode = 0x11;
constexpr uint8_t kCmdSwReset = 0x12;
constexpr uint8_t kCmdTemperatureSensorSelect = 0x18;
constexpr uint8_t kCmdMasterActivate = 0x20;
constexpr uint8_t kCmdDisplayUpdateControl1 = 0x21;
constexpr uint8_t kCmdDisplayUpdateControl2 = 0x22;
constexpr uint8_t kCmdSetRamXAddress = 0x44;
constexpr uint8_t kCmdSetRamYAddress = 0x45;
constexpr uint8_t kCmdSetRamXCounter = 0x4E;
constexpr uint8_t kCmdSetRamYCounter = 0x4F;
constexpr uint8_t kCmdBorderWaveformFull = 0x3C;
constexpr uint8_t kCmdWriteRamNew = 0x24;  // "current" image plane
constexpr uint8_t kCmdWriteRamOld = 0x26;  // "previous" image plane -- the

// SSD1677 computes its full-refresh waveform from the difference between
// these two planes. Writing only kCmdWriteRamNew (as an earlier version of
// this driver did) leaves the old plane stale/undefined, so the controller
// sees no meaningful change and the SPI transfer completes with no visible
// update -- confirmed on real hardware. A full refresh always writes the
// same framebuffer to both planes.
constexpr uint8_t kBoosterSoftStart[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
constexpr uint8_t kDataEntryModeFull = 0x01;
constexpr uint8_t kBorderWaveformFullValue = 0x01;
constexpr uint8_t kTemperatureSelectInternal = 0x80;
constexpr uint8_t kDisplayUpdateCtrl1Full[] = {0x40, 0x00};
constexpr uint8_t kDisplayUpdateCtrl2Full = 0xF7;

spi_device_handle_t s_spi = nullptr;
uint8_t* s_framebuffer = nullptr;  // 1bpp, BOARD_EPD_BUFFER_LEN bytes, 1 = white

void SetDc(bool data) { gpio_set_level(BOARD_EPD_DC_PIN, data ? 1 : 0); }

// The SPI DMA engine caps a single transaction at SPI_LL_DMA_MAX_BIT_LEN
// (32768 bytes on ESP32-S3) -- confirmed on real hardware, where sending the
// full ~48000-byte framebuffer in one shot failed with "txdata transfer >
// hardware max supported len". Chunk anything larger.
constexpr size_t kMaxSpiChunkBytes = 4096;

esp_err_t SpiSend(const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        const size_t chunk_len = std::min(kMaxSpiChunkBytes, len - offset);
        spi_transaction_t transaction = {};
        transaction.length = chunk_len * 8;
        transaction.tx_buffer = data + offset;
        const esp_err_t err = spi_device_transmit(s_spi, &transaction);
        if (err != ESP_OK) {
            return err;
        }
        offset += chunk_len;
    }
    return ESP_OK;
}

esp_err_t SendCommand(uint8_t command) {
    SetDc(false);
    return SpiSend(&command, 1);
}

esp_err_t SendData(const uint8_t* data, size_t len) {
    SetDc(true);
    return SpiSend(data, len);
}

esp_err_t SendDataByte(uint8_t byte) { return SendData(&byte, 1); }

esp_err_t SendCommandWithData(uint8_t command, const uint8_t* data, size_t len) {
    esp_err_t err = SendCommand(command);
    if (err != ESP_OK) {
        return err;
    }
    return SendData(data, len);
}

void WaitWhileBusy() {
    uint32_t waited_ms = 0;
    while (gpio_get_level(BOARD_EPD_BUSY_PIN) == 1 && waited_ms < kBusyTimeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(kBusyPollIntervalMs));
        waited_ms += kBusyPollIntervalMs;
    }
    if (waited_ms >= kBusyTimeoutMs) {
        ESP_LOGW(kTag, "BUSY pin timed out after %lums", static_cast<unsigned long>(waited_ms));
    }
}

void HardwareReset() {
    gpio_set_level(BOARD_EPD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_err_t ConfigureGpio() {
    gpio_config_t out_cfg = {};
    out_cfg.pin_bit_mask = (1ULL << BOARD_EPD_DC_PIN) | (1ULL << BOARD_EPD_RST_PIN) |
                           (1ULL << BOARD_EPD_POWER_EN_PIN);
    out_cfg.mode = GPIO_MODE_OUTPUT;
    esp_err_t err = gpio_config(&out_cfg);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t in_cfg = {};
    in_cfg.pin_bit_mask = 1ULL << BOARD_EPD_BUSY_PIN;
    in_cfg.mode = GPIO_MODE_INPUT;
    return gpio_config(&in_cfg);
}

esp_err_t ConfigureSpi() {
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = BOARD_EPD_MOSI_PIN;
    bus_cfg.miso_io_num = -1;  // write-only panel; MISO unused by this driver
    bus_cfg.sclk_io_num = BOARD_EPD_SCK_PIN;
    bus_cfg.max_transfer_sz = BOARD_EPD_BUFFER_LEN;
    esp_err_t err = spi_bus_initialize(BOARD_EPD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = kSpiClockHz;
    dev_cfg.mode = 0;
    dev_cfg.spics_io_num = BOARD_EPD_CS_PIN;
    dev_cfg.queue_size = 1;
    return spi_bus_add_device(BOARD_EPD_SPI_HOST, &dev_cfg, &s_spi);
}

esp_err_t SetWindow(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end) {
    const uint8_t ram_x_window[] = {
        static_cast<uint8_t>(x_start & 0xFF),
        static_cast<uint8_t>(x_start >> 8),
        static_cast<uint8_t>(x_end & 0xFF),
        static_cast<uint8_t>(x_end >> 8),
    };
    const uint8_t ram_y_window[] = {
        static_cast<uint8_t>(y_start & 0xFF),
        static_cast<uint8_t>(y_start >> 8),
        static_cast<uint8_t>(y_end & 0xFF),
        static_cast<uint8_t>(y_end >> 8),
    };
    esp_err_t err = SendCommandWithData(kCmdSetRamXAddress, ram_x_window, sizeof(ram_x_window));
    if (err != ESP_OK) {
        return err;
    }
    return SendCommandWithData(kCmdSetRamYAddress, ram_y_window, sizeof(ram_y_window));
}

esp_err_t SetCursor(uint16_t x_start, uint16_t y_start) {
    const uint8_t cursor_x[] = {static_cast<uint8_t>(x_start & 0xFF),
                                static_cast<uint8_t>(x_start >> 8)};
    const uint8_t cursor_y[] = {static_cast<uint8_t>(y_start & 0xFF),
                                static_cast<uint8_t>(y_start >> 8)};
    esp_err_t err = SendCommandWithData(kCmdSetRamXCounter, cursor_x, sizeof(cursor_x));
    if (err != ESP_OK) {
        return err;
    }
    return SendCommandWithData(kCmdSetRamYCounter, cursor_y, sizeof(cursor_y));
}

// Full re-init before every full refresh (including a hardware reset) --
// this SSD1677 needs that to produce a clean waveform each time, per the
// ported reference driver. Window/cursor: X spans [0, width-1] in pixels
// (NOT bytes -- confirmed from the reference, a wrong assumption in an
// earlier version of this file cost real debugging time), Y spans
// [height-1, 0] to match data-entry mode 0x01 (Y decrements).
esp_err_t InitFull() {
    HardwareReset();
    WaitWhileBusy();

    esp_err_t err = SendCommand(kCmdSwReset);
    if (err != ESP_OK) return err;
    WaitWhileBusy();

    const uint16_t rows_minus_one = BOARD_EPD_HEIGHT - 1;
    const uint8_t driver_output[] = {
        static_cast<uint8_t>(rows_minus_one & 0xFF),
        static_cast<uint8_t>(rows_minus_one >> 8),
        0x02,
    };

    err = SendCommandWithData(kCmdBoosterSoftStart, kBoosterSoftStart, sizeof(kBoosterSoftStart));
    if (err != ESP_OK) return err;
    err = SendCommandWithData(kCmdDriverOutputControl, driver_output, sizeof(driver_output));
    if (err != ESP_OK) return err;
    err = SendCommandWithData(kCmdDataEntryMode, &kDataEntryModeFull, 1);
    if (err != ESP_OK) return err;
    err = SetWindow(0, BOARD_EPD_HEIGHT - 1, BOARD_EPD_WIDTH - 1, 0);
    if (err != ESP_OK) return err;
    err = SendCommandWithData(kCmdBorderWaveformFull, &kBorderWaveformFullValue, 1);
    if (err != ESP_OK) return err;
    err = SendCommandWithData(kCmdTemperatureSensorSelect, &kTemperatureSelectInternal, 1);
    if (err != ESP_OK) return err;
    return SetCursor(0, BOARD_EPD_HEIGHT - 1);
}

esp_err_t TurnOnDisplay() {
    esp_err_t err = SendCommandWithData(kCmdDisplayUpdateControl1, kDisplayUpdateCtrl1Full,
                                        sizeof(kDisplayUpdateCtrl1Full));
    if (err != ESP_OK) return err;
    err = SendCommandWithData(kCmdDisplayUpdateControl2, &kDisplayUpdateCtrl2Full, 1);
    if (err != ESP_OK) return err;
    err = SendCommand(kCmdMasterActivate);
    if (err != ESP_OK) return err;
    WaitWhileBusy();
    return ESP_OK;
}

}  // namespace

esp_err_t Init() {
    esp_err_t err = ConfigureGpio();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "gpio config failed: %s", esp_err_to_name(err));
        return err;
    }

    gpio_set_level(BOARD_EPD_POWER_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    err = ConfigureSpi();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "spi config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_framebuffer == nullptr) {
        s_framebuffer = static_cast<uint8_t*>(heap_caps_malloc(BOARD_EPD_BUFFER_LEN,
                                                                MALLOC_CAP_SPIRAM));
        if (s_framebuffer == nullptr) {
            ESP_LOGE(kTag, "failed to allocate %d byte framebuffer", BOARD_EPD_BUFFER_LEN);
            return ESP_ERR_NO_MEM;
        }
    }
    Clear(false);

    ESP_LOGI(kTag, "initialized: %dx%d", BOARD_EPD_WIDTH, BOARD_EPD_HEIGHT);
    return ESP_OK;
}

void Clear(bool black) {
    if (s_framebuffer == nullptr) {
        return;
    }
    memset(s_framebuffer, black ? 0x00 : 0xFF, BOARD_EPD_BUFFER_LEN);
}

void SetPixel(int x, int y, bool black) {
    if (s_framebuffer == nullptr || x < 0 || y < 0 || x >= BOARD_EPD_WIDTH ||
        y >= BOARD_EPD_HEIGHT) {
        return;
    }
    const size_t byte_index = (static_cast<size_t>(y) * BOARD_EPD_WIDTH + x) / 8;
    const uint8_t bit_mask = 0x80 >> (x % 8);
    if (black) {
        s_framebuffer[byte_index] &= ~bit_mask;
    } else {
        s_framebuffer[byte_index] |= bit_mask;
    }
}

esp_err_t FullRefresh() {
    if (s_framebuffer == nullptr || s_spi == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = InitFull();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "InitFull failed: %s", esp_err_to_name(err));
        return err;
    }

    // Both planes get the same (new) image: a full refresh establishes a
    // clean baseline, there is no real "previous" frame to diff against.
    err = SendCommand(kCmdWriteRamNew);
    if (err != ESP_OK) return err;
    err = SendData(s_framebuffer, BOARD_EPD_BUFFER_LEN);
    if (err != ESP_OK) return err;

    err = SendCommand(kCmdWriteRamOld);
    if (err != ESP_OK) return err;
    err = SendData(s_framebuffer, BOARD_EPD_BUFFER_LEN);
    if (err != ESP_OK) return err;

    return TurnOnDisplay();
}

}  // namespace epaper_display
