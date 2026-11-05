#pragma once

#include "esp_err.h"

// Polls the AI/record, Up, and Down buttons (all active-low, debounced).
// Deliberately simple (no click/double-click detection beyond a single
// hold-duration threshold the caller applies itself) since this project
// only needs hold-to-record on one button and plain press/release on the
// other two.
namespace button_service {

enum class ButtonId {
    kAi,
    kUp,
    kDown,
};

using PressStartHandler = void (*)(ButtonId id, void* context);
using PressEndHandler = void (*)(ButtonId id, uint32_t held_ms, void* context);

esp_err_t Init();
void SetHandlers(PressStartHandler on_start, PressEndHandler on_end, void* context);

}  // namespace button_service
