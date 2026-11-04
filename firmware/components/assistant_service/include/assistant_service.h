#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "esp_err.h"

// POSTs a WAV clip to the backend's /converse endpoint and parses the
// {"transcript": ..., "reply": ...} JSON response. The backend (see
// backend/app.py) does STT + the actual Claude call; this device only ever
// talks to the Pi/ZimaOS proxy, never to Anthropic directly.
namespace assistant_service {

struct ConverseResult {
    bool success = false;
    std::string transcript;
    std::string reply;
    int http_status = 0;
    std::string error_message;
};

ConverseResult Converse(const uint8_t* wav_bytes, size_t wav_len);

}  // namespace assistant_service
