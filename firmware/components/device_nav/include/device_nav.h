#pragma once

#include "esp_err.h"

// Up/Down button navigation across plugin device-views (fetched from the
// backend's GET /api/device/views -- see backend/plugins/base.py's
// DeviceView contract) plus the device's own home/conversation screen.
//
// Down moves a highlighted row forward within the current view; past the
// last row (or in a view with no rows), it advances to the next view,
// wrapping past the last plugin view back to home. Up mirrors this
// backward, landing on the *previous* view's *last* row so paging up
// through content feels continuous rather than jumping to a fixed spot.
// Confirm (a short tap of the AI button, handled by main.cpp since that's
// also the record button) toggles the highlighted row if the current view
// has one.
//
// Network calls (fetching views, posting a toggle) never run on the
// caller's thread -- OnUp/OnDown/OnConfirm must stay fast since they're
// called directly from button_service's poll task, same constraint that's
// why main.cpp hands recording off to a separate AssistantTask rather than
// posting /converse inline.
namespace device_nav {

esp_err_t Init();

// Called when a real recording starts (a genuine long-press, not a tap),
// which takes over the screen for the rest of the converse flow -- so the
// next Up/Down press after that starts fresh from home rather than
// resuming wherever paging had left off before the button did double
// duty as record.
void ResetToHome();

void OnUp();
void OnDown();

// Returns true if a device view consumed the tap (toggled a row, and is
// still showing); false if there was nothing to confirm -- the home
// screen is showing, or the current view has no checkable row highlighted
// -- so the caller should fall back to its own idle-screen behavior.
bool OnConfirm();

}  // namespace device_nav
