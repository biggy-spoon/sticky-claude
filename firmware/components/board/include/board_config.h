// reTerminal Sticky (ESP32-S3R8) pin map, scoped to what this project uses:
// the AI/record + Up/Down buttons, the PDM microphone, the SSD1677 ePaper
// panel, and (as of touch_probe) the capacitive touch panel's I2C bus.
// SD card and the fuel-gauge/IMU sensors are on this board too but are not
// wired up here since this project doesn't use them.
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

#define BOARD_BUTTON_AI_PIN      GPIO_NUM_4   // long-press = record, short tap = confirm/toggle
// Confirmed from Seeed's own hardware-overview docs
// (seeedstudio.com/sticky/docs/en/device-guide/hardware-overview, "Physical
// buttons" row) and independently corroborated by a CNX Software review of
// the product -- both list Up/Down as real buttons distinct from the AI
// button. Not wired up by this project until the on-device plugin-view
// navigation feature; electrical convention (active-low, pull-up) assumed
// to match the AI button's and needs confirming on real hardware.
#define BOARD_BUTTON_UP_PIN      GPIO_NUM_5   // previous device view / row
#define BOARD_BUTTON_DOWN_PIN    GPIO_NUM_6   // next device view / row

#define BOARD_PDM_CLK_PIN        GPIO_NUM_19
#define BOARD_PDM_DATA_PIN       GPIO_NUM_20
#define BOARD_PDM_POWER_EN_PIN   GPIO_NUM_38
#define BOARD_PDM_POWER_ACTIVE_LEVEL 1

#define BOARD_EPD_SPI_HOST       SPI2_HOST
#define BOARD_EPD_POWER_EN_PIN   GPIO_NUM_47
#define BOARD_EPD_BUSY_PIN       GPIO_NUM_18
#define BOARD_EPD_RST_PIN        GPIO_NUM_17
#define BOARD_EPD_DC_PIN         GPIO_NUM_16
#define BOARD_EPD_CS_PIN         GPIO_NUM_15
#define BOARD_EPD_MOSI_PIN       GPIO_NUM_14
#define BOARD_EPD_MISO_PIN       GPIO_NUM_12
#define BOARD_EPD_SCK_PIN        GPIO_NUM_13

#define BOARD_EPD_WIDTH          800
#define BOARD_EPD_HEIGHT         480
#define BOARD_EPD_BUFFER_LEN     ((BOARD_EPD_WIDTH * BOARD_EPD_HEIGHT) / 8)

// Touch panel I2C. Pins are confirmed from Seeed's hardware-overview docs
// (SCL/SDA/INT/RST/EN); the controller chip is not named anywhere Seeed
// publishes. The schematic's I2C-tree label claimed address 0x6A, but a
// real-hardware bus scan found nothing there and instead found a device
// at 0x14 -- one of the GT911's two selectable addresses (chosen by the
// INT pin's level during reset; ours floats high via its pull-up, which
// is the documented condition for 0x14). Treating it as a GT911 until
// proven otherwise; see touch_probe for the protocol-level verification.
#define BOARD_TP_I2C_SCL_PIN     GPIO_NUM_2
#define BOARD_TP_I2C_SDA_PIN     GPIO_NUM_3
#define BOARD_TP_INT_PIN         GPIO_NUM_21
#define BOARD_TP_RST_PIN         GPIO_NUM_41
#define BOARD_TP_PWR_EN_PIN      GPIO_NUM_42
#define BOARD_TP_I2C_ADDR        0x14
