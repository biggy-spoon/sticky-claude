#include "pdm_mic.h"

#include "board_config.h"
#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace pdm_mic {
namespace {

constexpr const char* kTag = "pdm_mic";
constexpr uint32_t kPowerSettleDelayMs = 10;
constexpr size_t kDmaDescNum = 6;
constexpr size_t kDmaFrameNum = 480;  // 30ms per descriptor at 16kHz

i2s_chan_handle_t s_rx_handle = nullptr;
bool s_enabled = false;

esp_err_t ConfigurePowerPin() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << BOARD_PDM_POWER_EN_PIN;
    cfg.mode = GPIO_MODE_OUTPUT;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_level(BOARD_PDM_POWER_EN_PIN, !BOARD_PDM_POWER_ACTIVE_LEVEL);
}

}  // namespace

esp_err_t Init() {
    if (s_rx_handle != nullptr) {
        return ESP_OK;
    }

    esp_err_t err = ConfigurePowerPin();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "power pin config failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = kDmaDescNum;
    chan_cfg.dma_frame_num = kDmaFrameNum;
    err = i2s_new_channel(&chan_cfg, nullptr, &s_rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_new_channel failed: %s", esp_err_to_name(err));
        s_rx_handle = nullptr;
        return err;
    }

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(kSampleRateHz),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                   I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = BOARD_PDM_CLK_PIN,
            .din = BOARD_PDM_DATA_PIN,
        },
    };

    err = i2s_channel_init_pdm_rx_mode(s_rx_handle, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_channel_init_pdm_rx_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_handle);
        s_rx_handle = nullptr;
        return err;
    }

    ESP_LOGI(kTag, "initialized: rate=%lu clk=GPIO%d data=GPIO%d",
             static_cast<unsigned long>(kSampleRateHz),
             static_cast<int>(BOARD_PDM_CLK_PIN), static_cast<int>(BOARD_PDM_DATA_PIN));
    return ESP_OK;
}

esp_err_t Enable() {
    if (s_rx_handle == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_enabled) {
        return ESP_OK;
    }

    gpio_set_level(BOARD_PDM_POWER_EN_PIN, BOARD_PDM_POWER_ACTIVE_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(kPowerSettleDelayMs));

    esp_err_t err = i2s_channel_enable(s_rx_handle);
    if (err != ESP_OK) {
        gpio_set_level(BOARD_PDM_POWER_EN_PIN, !BOARD_PDM_POWER_ACTIVE_LEVEL);
        return err;
    }
    s_enabled = true;
    return ESP_OK;
}

esp_err_t Disable() {
    if (s_rx_handle == nullptr || !s_enabled) {
        return ESP_OK;
    }
    esp_err_t err = i2s_channel_disable(s_rx_handle);
    gpio_set_level(BOARD_PDM_POWER_EN_PIN, !BOARD_PDM_POWER_ACTIVE_LEVEL);
    s_enabled = false;
    return err;
}

esp_err_t Read(int16_t* dest, size_t sample_count, size_t* samples_read, uint32_t timeout_ms) {
    if (dest == nullptr || sample_count == 0 || samples_read == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_enabled) {
        *samples_read = 0;
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_handle, dest, sample_count * sizeof(int16_t),
                                     &bytes_read, timeout_ms);
    *samples_read = bytes_read / sizeof(int16_t);
    return err;
}

}  // namespace pdm_mic
