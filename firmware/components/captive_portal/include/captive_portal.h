#pragma once

#include "esp_err.h"

// First-run setup flow: starts an open WiFi AP ("StickyClaude-Setup"), a DNS
// server that answers every query with the AP's own IP (the standard
// captive-portal trick so phones auto-open the setup page), and an HTTP
// server with a settings form. On submit, saves to settings_service and
// reboots into normal operation.
//
// Blocks (serving requests) until a save succeeds and the device reboots --
// callers should treat this as "does not return" on success.
namespace captive_portal {

esp_err_t Run();

}  // namespace captive_portal
