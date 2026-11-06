// reTerminal Sticky (ESP32-S3R8) pin map, scoped to what this project uses:
// the AI/record + Up/Down buttons, the PDM microphone, the SSD1677 ePaper
// panel, and the GT911 touch panel. SD card and the fuel-gauge/IMU sensors are
// on this board too but are not wired up here since this project doesn't use
// them.
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

// Board power latch (schematic PWR_HOLD/PWR_LOCK). Pin numbers and the
// drive-both-high behavior come from the handwriting pad firmware's
// board_power.cpp, not from Seeed's docs.
#define BOARD_POWER_HOLD_PIN     GPIO_NUM_45
#define BOARD_POWER_LOCK_PIN     GPIO_NUM_46

#define BOARD_BUTTON_AI_PIN      GPIO_NUM_4  // long-press = record, short tap = confirm/toggle
#define BOARD_BUTTON_UP_PIN     GPIO_NUM_5   // previous device view / row
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

// GT911 touch panel. 0x14 is selected by holding INT high across the reset
// edge. The schematic's I2C tree labels the touch panel 0x6A, but nothing
// answers there; a bus scan finds the chip at 0x14.
#define BOARD_TP_I2C_SCL_PIN    GPIO_NUM_2
#define BOARD_TP_I2C_SDA_PIN     GPIO_NUM_3
#define BOARD_TP_INT_PIN         GPIO_NUM_21
#define BOARD_TP_RST_PIN         GPIO_NUM_41
#define BOARD_TP_PWR_EN_PIN      GPIO_NUM_42
#define BOARD_TP_I2C_ADDR        0x14
