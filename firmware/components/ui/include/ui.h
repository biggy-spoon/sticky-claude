#pragma once

#include <string>

#include "esp_err.h"

// LVGL-based screens rendered to the ePaper panel through epaper_display's
// framebuffer. Screens only redraw on explicit state changes (never
// animated) -- e-paper full refresh takes a couple of seconds, so anything
// resembling continuous UI updates would be both pointless and slow.
namespace ui {

esp_err_t Init();

void ShowIdle(bool wifi_connected);
void ShowRecording();
void ShowThinking();
void ShowReply(const std::string& transcript, const std::string& reply);
void ShowError(const std::string& message);

}  // namespace ui
