#pragma once

#include "esp_err.h"

// Polls the single AI/record button (active-low, BOARD_BUTTON_AI_PIN) and
// calls back on press-start / press-end, debounced. Deliberately simple
// (no click/double-click detection, unlike full UI firmware) since this
// project only needs hold-to-record.
namespace button_service {

using PressStartHandler = void (*)(void* context);
using PressEndHandler = void (*)(uint32_t held_ms, void* context);

esp_err_t Init();
void SetHandlers(PressStartHandler on_start, PressEndHandler on_end, void* context);

}  // namespace button_service
