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

// One row of a device_nav-driven plugin view.
struct DeviceViewRow {
    std::string text;
    std::string secondary;  // optional smaller second line
    bool checkable = false;
    bool checked = false;  // drawn with a ticked box and struck-through text
};

// Where a screen sits in the page rotation, for the indicator dots. index 0 is
// home. A total of 1 or less hides the indicator.
struct PagePos {
    int index = 0;
    int total = 0;
};

esp_err_t Init();

// total_pages counts home itself. 0 means "not known yet".
void ShowIdle(bool wifi_connected, int total_pages = 0);
void ShowRecording();
void ShowThinking();
void ShowReply(const std::string& transcript, const std::string& reply, int total_pages = 0);
void ShowError(const std::string& message);
void ShowLoading();

// keep_scroll holds the list where it is, for redrawing after a row changes.
void ShowDeviceView(const std::string& title, const std::vector<DeviceViewRow>& rows,
                    PagePos page, bool keep_scroll);
void ShowDetail(const std::string& title, const std::string& body, PagePos page);

// Moves the plugin view or detail screen by about a page: +1 towards later
// content, -1 towards earlier. Ignored while a refresh is pending or running,
// so it can't replace a queued screen, and when already at that end, so it
// doesn't cost a needless refresh.
void Scroll(int direction);

// True while a refresh is pending or running: what's on the glass may not match
// the current state, so a tap can't be trusted to mean what it looks like.
bool IsBusy();

// Which row of the plugin view is at this screen position, or -1. Also -1
// while busy.
int HitTestDeviceViewRow(int x, int y);

}  // namespace ui
