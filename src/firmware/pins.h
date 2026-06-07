#pragma once

#if defined(BOARD_RETERMINAL)

// --- Seeed reTerminal E1001 (XIAO-ESP32-S3) ---
// Pin map from Seeed Arduino cookbook + Zephyr board port (corroborated).
// Display and microSD share one SPI bus (SCK/MOSI/MISO); they have separate CS.
#define BOARD_I2C_SDA       19
#define BOARD_I2C_SCL       20

#define BOARD_SPI_SCK       7
#define BOARD_SPI_MOSI      9
#define BOARD_SPI_MISO      8

#define BOARD_EPD_CS        10
#define BOARD_EPD_DC        11
#define BOARD_EPD_RST       12
#define BOARD_EPD_BUSY      13

#define BOARD_SD_CS         14
#define BOARD_SD_DET        15   // card-detect (active low)
#define BOARD_SD_EN         16   // drive HIGH to power the slot

// Front buttons (active-low, internal pull-up). GPIO4 doubles as deep-sleep wake.
#define BOARD_BTN_REFRESH   3
#define BOARD_BTN_1         4
#define BOARD_BTN_2         5

// Battery: no fuel gauge — read divider on ADC, gated by an enable line.
#define BOARD_BAT_ADC       1    // ADC1, x2.0 divider
#define BOARD_BAT_EN        21   // drive HIGH to power the divider before reading

#define BOARD_LED           6    // green, active-low
#define BOARD_BUZZER        45   // passive piezo (PWM)

#define BOARD_BOOT_PIN      0

#else

// --- LilyGo T-Deck Pro ---
#define BOARD_I2C_SDA       13
#define BOARD_I2C_SCL       14

#define BOARD_SPI_SCK       36
#define BOARD_SPI_MOSI      33
#define BOARD_SPI_MISO      47

#define BOARD_EPD_CS        34
#define BOARD_EPD_DC        35
#define BOARD_EPD_BUSY      37
#define BOARD_EPD_RST       -1

#define BOARD_LORA_CS       3
#define BOARD_LORA_RST      4
#define BOARD_LORA_DIO1     5
#define BOARD_LORA_BUSY     6
#define BOARD_SD_CS         48

#define BOARD_KEYBOARD_INT  15
#define BOARD_KEYBOARD_LED  42

#define BOARD_LORA_EN       46
#define BOARD_GPS_EN        39
#define BOARD_1V8_EN        38
#define BOARD_GPS_TXD       43
#define BOARD_GPS_RXD       44
#define BOARD_GPS_PPS       1

#define BOARD_MODEM_POWER_EN 41
#define BOARD_MODEM_PWRKEY   40
#define BOARD_MODEM_RST      9
#define BOARD_MODEM_RXD      10
#define BOARD_MODEM_TXD      11
#define BOARD_MODEM_DTR      8
#define BOARD_MODEM_RI       7

#define BOARD_TOUCH_INT     12
#define BOARD_TOUCH_RST     45

#define BOARD_BOOT_PIN      0

#endif
