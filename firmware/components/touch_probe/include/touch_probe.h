#pragma once

#include "esp_err.h"

// Throwaway diagnostic component for reverse-engineering the reTerminal
// Sticky's touch panel I2C protocol -- the controller chip isn't named
// anywhere Seeed publishes (see board_config.h), so this just dumps raw
// register bytes + the INT pin's level on a timer, for comparing against
// manual touches in the serial log. Not meant to ship -- once the real
// protocol is known, this gets replaced by an actual touch driver (and
// an LVGL input device), not extended.
namespace touch_probe {

esp_err_t Init();

}  // namespace touch_probe
