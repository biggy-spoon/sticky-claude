#pragma once

#include <string>
#include <vector>

#include "esp_err.h"

// LVGL-based screens rendered to the ePaper panel through epaper_display's
// framebuffer. A full refresh takes about two seconds, so every Show* call
// only queues the screen and returns at once: a dedicated task owns LVGL and
// the panel, and if several requests arrive during a refresh only the newest
// is drawn. Safe to call from any task.
namespace ui {

// One row of a device_nav-driven plugin view. device_nav owns the backing
// item ids and cursor logic.
struct DeviceViewRow {
    std::string text;
    std::string secondary;  // optional smaller second line
    bool checkable = false;
    bool checked = false;
    bool highlighted = false;
};

esp_err_t Init();

// total_pages counts home itself (1 = just home, nothing to page through).
// 0 means "not known yet" and hides the indicator, same as total_pages <= 1.
void ShowIdle(bool wifi_connected, int total_pages = 0);
void ShowRecording();
void ShowThinking();
void ShowReply(const std::string& transcript, const std::string& reply);
void ShowError(const std::string& message);
void ShowLoading();
void ShowDeviceView(const std::string& title, const std::vector<DeviceViewRow>& rows);

// Which row of the plugin view is at this screen position, or -1. Also -1
// while a refresh is pending or in progress: the rows on the glass may not
// match the current list then, and a tap could act on the wrong item.
int HitTestDeviceViewRow(int x, int y);

}  // namespace ui
