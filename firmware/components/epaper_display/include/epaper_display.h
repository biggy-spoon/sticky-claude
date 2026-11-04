#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Low-level driver for the board's SSD1677-controlled 800x480 monochrome
// ePaper panel: SPI transport, panel init, and a 1bpp framebuffer with a
// full-refresh write. Deliberately does NOT include text/font rendering —
// see firmware/README.md "Known gaps" for why and what to add.
//
// NOTE: the init/refresh command sequence here follows the standard SSD16xx
// command set from public datasheets/reference drivers, but has not been
// verified against a real reTerminal Sticky panel (no hardware to test on
// in this environment). Treat this as a bring-up starting point, not a
// known-working driver — budget time to debug timing/LUT/command order
// against the datasheet or a working reference (e.g. a logic analyzer trace,
// or diffing against another open-source driver for the same panel).
namespace epaper_display {

esp_err_t Init();

void Clear(bool black);
void SetPixel(int x, int y, bool black);

// Pushes the framebuffer to the panel and performs a full refresh. This is
// the panel's slow path (multiple seconds) -- there is no partial-refresh
// support here.
esp_err_t FullRefresh();

}  // namespace epaper_display
