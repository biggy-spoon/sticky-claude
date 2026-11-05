#pragma once

#include <string>
#include <vector>

#include "esp_err.h"

// LVGL-based screens rendered to the ePaper panel through epaper_display's
// framebuffer. Screens only redraw on explicit state changes (never
// animated) -- e-paper full refresh takes a couple of seconds, so anything
// resembling continuous UI updates would be both pointless and slow.
namespace ui {

// One row of a device_nav-driven plugin view (see device_nav.h). Plain
// display data only -- device_nav owns the backing item ids and cursor
// logic, ui just draws whatever it's told.
struct DeviceViewRow {
    std::string text;
    bool checkable = false;
    bool checked = false;
    bool highlighted = false;
};

esp_err_t Init();

// total_pages counts home itself (1 = just home, nothing to page
// through). 0 means "not known yet" (nothing fetched this boot) and
// hides the indicator entirely, same as total_pages <= 1.
void ShowIdle(bool wifi_connected, int total_pages = 0);
void ShowRecording();
void ShowThinking();
void ShowReply(const std::string& transcript, const std::string& reply);
void ShowError(const std::string& message);

// A plugin's on-device screen (see device_nav.h), and the brief loading
// state shown while fetching one.
void ShowLoading();
void ShowDeviceView(const std::string& title, const std::vector<DeviceViewRow>& rows);

}  // namespace ui
