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

// SSD1677-family command opcodes (standard SSD16xx set). Unverified against
// real hardware -- see header comment.
constexpr uint8_t kCmdSwReset = 0x12;
constexpr uint8_t kCmdDriverOutputControl = 0x01;
constexpr uint8_t kCmdDataEntryMode = 0x11;
constexpr uint8_t kCmdSetRamXAddress = 0x44;
constexpr uint8_t kCmdSetRamYAddress = 0x45;
constexpr uint8_t kCmdSetRamXCounter = 0x4E;
constexpr uint8_t kCmdSetRamYCounter = 0x4F;
constexpr uint8_t kCmdBorderWaveform = 0x3C;
constexpr uint8_t kCmdDisplayUpdateControl2 = 0x22;
constexpr uint8_t kCmdMasterActivate = 0x20;
constexpr uint8_t kCmdWriteRamBw = 0x24;

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
    vTaskDelay(pdMS_TO_TICKS(10));
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

// Standard SSD16xx bring-up sequence: reset, driver output control (panel
// height), data entry mode, full-window RAM address range, border waveform,
// and the "clear display" update sequence. See header note: unverified.
esp_err_t PanelInitSequence() {
    HardwareReset();
    WaitWhileBusy();

    esp_err_t err = SendCommand(kCmdSwReset);
    if (err != ESP_OK) return err;
    WaitWhileBusy();

    err = SendCommand(kCmdDriverOutputControl);
    if (err != ESP_OK) return err;
    const uint16_t rows_minus_one = BOARD_EPD_HEIGHT - 1;
    SendDataByte(rows_minus_one & 0xFF);
    SendDataByte((rows_minus_one >> 8) & 0xFF);
    SendDataByte(0x00);

    err = SendCommand(kCmdDataEntryMode);
    if (err != ESP_OK) return err;
    SendDataByte(0x03);  // X increment, Y increment

    err = SendCommand(kCmdSetRamXAddress);
    if (err != ESP_OK) return err;
    SendDataByte(0x00);
    SendDataByte((BOARD_EPD_WIDTH / 8) - 1);

    err = SendCommand(kCmdSetRamYAddress);
    if (err != ESP_OK) return err;
    SendDataByte(0x00);
    SendDataByte(0x00);
    SendDataByte((BOARD_EPD_HEIGHT - 1) & 0xFF);
    SendDataByte(((BOARD_EPD_HEIGHT - 1) >> 8) & 0xFF);

    err = SendCommand(kCmdBorderWaveform);
    if (err != ESP_OK) return err;
    SendDataByte(0x05);

    err = SendCommand(kCmdSetRamXCounter);
    if (err != ESP_OK) return err;
    SendDataByte(0x00);

    err = SendCommand(kCmdSetRamYCounter);
    if (err != ESP_OK) return err;
    SendDataByte(0x00);
    SendDataByte(0x00);

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

    err = PanelInitSequence();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "panel init sequence failed: %s", esp_err_to_name(err));
        return err;
    }

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

    esp_err_t err = SendCommand(kCmdWriteRamBw);
    if (err != ESP_OK) {
        return err;
    }
    err = SendData(s_framebuffer, BOARD_EPD_BUFFER_LEN);
    if (err != ESP_OK) {
        return err;
    }

    err = SendCommand(kCmdDisplayUpdateControl2);
    if (err != ESP_OK) return err;
    SendDataByte(0xF7);  // full display update sequence

    err = SendCommand(kCmdMasterActivate);
    if (err != ESP_OK) return err;
    WaitWhileBusy();

    return ESP_OK;
}

}  // namespace epaper_display
