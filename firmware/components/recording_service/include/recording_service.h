#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Captures mic audio into a PSRAM buffer for the duration of a button hold,
// then hands back a ready-to-upload WAV byte buffer. One clip at a time;
// no pre-roll buffering (see README "Known gaps" for why that's a possible
// follow-up: the first ~50ms of speech right at button-press can get clipped
// while the capture task spins up).
namespace recording_service {

constexpr uint32_t kMaxRecordingMs = 15000;

esp_err_t Init();

// Starts filling the clip buffer from the mic. Non-blocking.
esp_err_t StartCapture();

// Stops capture and returns the number of samples actually recorded.
esp_err_t StopCapture(size_t* out_sample_count);

// Builds a 44-byte-header PCM16 mono WAV from the last captured clip into a
// buffer this function allocates with malloc(). Caller must free() it.
esp_err_t BuildWav(uint8_t** out_wav, size_t* out_wav_len);

}  // namespace recording_service
