#pragma once

#include "esp_err.h"

// Moves between the device's home screen and the plugin views fetched from the
// backend's GET /api/device/views (see backend/plugins/base.py's DeviceView
// contract). Buttons and horizontal swipes change page, wrapping past either
// end. Vertical swipes scroll the current page. Tapping a row toggles it if
// it's checkable, or opens its details if it has any.
//
// Pages draw from a cached copy of the views and refresh in the background, so
// changing page doesn't wait on the network. Network calls run on this
// component's own task, so every function here is quick enough for the button
// and touch tasks.
namespace device_nav {

esp_err_t Init();

// Called once after the initial WiFi connect attempt. Used so the home screen
// reports accurate status.
void SetWifiConnected(bool connected);

// Fetches the views once at startup and then shows the home screen, so its page
// indicator is right without paging first.
void Preload();

// Plugin views last fetched (not counting home, which is the firmware's own
// screen), or -1 if nothing has been fetched yet this boot.
int KnownPluginViewCount();

// While the AI button is down, and for the whole conversation after a real
// recording, other input is ignored and background refreshes don't draw, so
// nothing replaces the conversation screens.
void PauseInput();
void ResumeInput();

// A recording is going ahead: the next page change starts from home.
void ResetToHome();

// The conversation is over and its result is about to go on screen. Resumes
// input and refreshes the cached views, since Claude may have changed them.
void FinishConversation();

// +1 for the next page, -1 for the previous.
void Navigate(int direction);

// +1 towards later content, -1 towards earlier.
void OnScroll(int direction);

// A tap at screen coordinates. Ignored while the screen is refreshing, since
// what's on the glass may not match the list yet.
void OnTap(int x, int y);

// A short press of the AI button: details go back to their list, a plugin view
// goes home, and home dismisses any reply or error.
void OnBack();

}  // namespace device_nav
