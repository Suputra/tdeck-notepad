#pragma once

// Per-board capability flags.
//
// The build env defines BOARD_RETERMINAL for the Seeed reTerminal E1001;
// the default (no flag) is the LilyGo T-Deck Pro. Device-specific code keys
// off the HAS_* macros below rather than sprinkling #ifdef BOARD_* everywhere,
// so call sites read semantically.

#if defined(BOARD_RETERMINAL)

// --- Seeed reTerminal E1001 (XIAO-ESP32-S3, 7.5" 800x480 e-paper) ---
#define DEVICE_DISPLAY_NAME       "reTerminal-E1001"
#define HAS_MATRIX_KEYBOARD       0   // no physical keyboard
#define HAS_TOUCH                 0   // no touch panel on the E1001
#define HAS_MODEM                 0
#define HAS_GNSS                  0
#define HAS_MESHTASTIC            0
#define HAS_LORA                  0
#define HAS_BLE_HID               0   // device is NOT a BLE keyboard/mouse
#define HAS_BLE_INPUT             1   // device RECEIVES keystrokes over BLE GATT
#define HAS_HW_BUTTONS            1   // 3 front buttons
#define HAS_FUEL_GAUGE            0   // battery read via ADC divider, not a gauge
#define BLE_DEFAULT_ON            1   // BLE input is the only input source
#define DISPLAY_DRIVER_RETERMINAL 1
#define DEFAULT_BT_NAME           "s-term-rt"

#else

// --- LilyGo T-Deck Pro (default) ---
#define DEVICE_DISPLAY_NAME       "TDeck-Pro"
#define HAS_MATRIX_KEYBOARD       1
#define HAS_TOUCH                 1
#define HAS_MODEM                 1
#define HAS_GNSS                  1
#define HAS_MESHTASTIC            1
#define HAS_LORA                  1
#define HAS_BLE_HID               1
#define HAS_BLE_INPUT             0
#define HAS_HW_BUTTONS            0
#define HAS_FUEL_GAUGE            1
#define BLE_DEFAULT_ON            0
#define DISPLAY_DRIVER_RETERMINAL 0
#define DEFAULT_BT_NAME           "TDeck-Pro"

#endif
