#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Thin wrapper around the ESP-IDF I2S PDM RX driver for the board's single
// PDM microphone. Mono, 16-bit, 16kHz by default (matches what Whisper and
// Claude both expect speech audio at, so no resampling is needed downstream).
namespace pdm_mic {

constexpr uint32_t kSampleRateHz = 16000;

esp_err_t Init();
esp_err_t Enable();
esp_err_t Disable();

// Blocking read of up to `sample_count` int16 samples. Returns ESP_ERR_TIMEOUT
// if nothing arrived within `timeout_ms` (not treated as a hard error by callers).
esp_err_t Read(int16_t* dest, size_t sample_count, size_t* samples_read, uint32_t timeout_ms);

}  // namespace pdm_mic
