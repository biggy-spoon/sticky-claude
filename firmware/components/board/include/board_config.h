// reTerminal Sticky (ESP32-S3R8) pin map, scoped to what this project uses:
// the AI/record button, the PDM microphone, and the SSD1677 ePaper panel.
// Touch, SD card, and the fuel-gauge/IMU sensors are on this board too but are
// not wired up here since this project doesn't use them.
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

#define BOARD_BUTTON_AI_PIN      GPIO_NUM_4   // long-press = record

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
