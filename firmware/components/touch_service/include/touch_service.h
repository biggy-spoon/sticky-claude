#pragma once

#include "esp_err.h"

// GT911 capacitive touch, reported as taps in screen coordinates (800x480,
// origin top-left with the display's text upright). Polled from its own task.
namespace touch_service {

using TapHandler = void (*)(int x, int y, void* context);

// Set the handler before Init(). It runs on the touch task, so it must not
// block for long.
void SetTapHandler(TapHandler handler, void* context);

// A failure here means the chip never came up. The rest of the device works
// without touch, so callers should log it rather than abort.
esp_err_t Init();

}  // namespace touch_service
