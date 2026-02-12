#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <GxEPD2_BW.h>
#include <Adafruit_TCA8418.h>
#include <WiFi.h>
#include <libssh/libssh.h>
#include <esp_task_wdt.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <esp_netif.h>
#include <lwip/dns.h>
#include <SD.h>
#include <FS.h>
#include <WireGuard-ESP32.h>

#include "firmware/pins.h"
#include "firmware/layout.h"
#include "firmware/keyboard_map.h"
#include "firmware/network_config.h"

// --- WiFi / SSH Config (loaded from SD /CONFIG) ---

static WiFiAP config_wifi[MAX_WIFI_APS];
static int config_wifi_count = 0;
static char config_ssh_host[64]   = "";
static char config_ssh_vpn_host[64] = "";
static int  config_ssh_port       = 22;
static char config_ssh_user[64]   = "";
static char config_ssh_pass[64]   = "";

// --- Bluetooth Config (loaded from SD /CONFIG) ---
static bool     config_bt_enabled = false;
static char     config_bt_name[32] = "TDeck-Pro";
static uint32_t config_bt_passkey = 0; // 6-digit optional static PIN

// --- VPN Config (loaded from SD /CONFIG) ---
static char   config_vpn_privkey[64] = "";
static char   config_vpn_pubkey[64]  = "";
static char   config_vpn_psk[64]     = "";
static char   config_vpn_ip[32]      = "";
static char   config_vpn_endpoint[64] = "";
static int    config_vpn_port        = 51820;
static char   config_vpn_dns[32]     = "";
static WireGuard wg;
static bool   vpn_connected = false;
static bool vpnConfigured() { return config_vpn_privkey[0] != '\0'; }
static bool vpnActive() { return vpn_connected && wg.is_initialized(); }

// Forward declarations
void connectMsg(const char* fmt, ...);
void powerOff();
static int partial_count = 0;

// --- SD Card State ---
static bool sd_mounted = false;
static volatile bool sd_busy = false;      // display task yields when true
static volatile bool display_idle = true;  // false while display is doing SPI

// --- Display ---

GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT> display(
    GxEPD2_310_GDEQ031T10(BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST, BOARD_EPD_BUSY)
);

// --- Keyboard ---

Adafruit_TCA8418 keypad;

// --- App Mode ---

enum AppMode { MODE_NOTEPAD, MODE_TERMINAL, MODE_KEYBOARD, MODE_COMMAND };
static volatile AppMode app_mode = MODE_NOTEPAD;

// --- Editor State (shared between cores, protected by mutex) ---

// --- Command Processor State ---
#define CMD_BUF_LEN 64
static char cmd_buf[CMD_BUF_LEN + 1];
static int  cmd_len = 0;
static AppMode cmd_return_mode = MODE_NOTEPAD;  // Mode to return to after command
static AppMode keyboard_return_mode = MODE_NOTEPAD;

// Multi-line command result (half screen)
#define CMD_RESULT_LINES 13
static char cmd_result[CMD_RESULT_LINES][COLS_PER_LINE + 1];
static int  cmd_result_count = 0;
static bool cmd_result_valid = false;

// --- File State ---
static String current_file = "";
static bool file_modified = false;

static SemaphoreHandle_t state_mutex;
static volatile bool render_requested = false;
static volatile bool poweroff_requested = false;
static unsigned long boot_pressed_since = 0;
static bool boot_sleep_latched = false;
static constexpr unsigned long BOOT_SLEEP_HOLD_MS = 0;

// Editor state — written by core 1 (keyboard), read by core 0 (display)
static char text_buf[MAX_TEXT_LEN + 1];
static int  text_len    = 0;
static int  cursor_pos  = 0;
static int  scroll_line = 0;
static bool shift_held  = false;
static bool sym_mode    = false;
static bool alt_mode    = false;   // Ctrl modifier in terminal, unused in notepad
static bool nav_mode    = false;   // WASD arrow keys (right shift toggle)

// Display task snapshot — private to core 0
static char snap_buf[MAX_TEXT_LEN + 1];
static int  snap_len       = 0;
static int  snap_cursor    = 0;
static int  snap_scroll    = 0;
static bool snap_shift     = false;
static bool snap_sym       = false;
static bool snap_alt       = false;
static bool snap_nav       = false;

// --- Terminal State (shared, protected by state_mutex) ---

static char term_buf[TERM_ROWS][TERM_COLS + 1];
static int  term_line_count = 1;
static int  term_scroll     = 0;
static int  term_cursor_row = 0;
static int  term_cursor_col = 0;
static volatile bool term_render_requested = false;

// Terminal snapshot — private to core 0
static char term_snap_buf[TERM_ROWS][TERM_COLS + 1];
static int  term_snap_lines  = 1;
static int  term_snap_scroll = 0;
static int  term_snap_crow   = 0;
static int  term_snap_ccol   = 0;

// ANSI escape parser state
static bool in_escape  = false;
static bool in_bracket = false;
#define MAX_CSI_PARAMS 8
static int  csi_params[MAX_CSI_PARAMS];
static int  csi_param_count = 0;
static bool csi_parsing_num = false;
static bool csi_private = false;      // '?' seen in CSI params

// Scroll region (0-based screen rows)
static int scroll_region_top = 0;
static int scroll_region_bot = ROWS_PER_SCREEN - 1;
static bool scroll_region_set = false;  // explicitly set by CSI r

// Alternate screen buffer
static char term_alt_buf[TERM_ROWS][TERM_COLS + 1];
static bool term_alt_active = false;
static int saved_main_cursor_row = 0, saved_main_cursor_col = 0;
static int saved_main_line_count = 0, saved_main_scroll = 0;

// Cursor save/restore (ESC 7/8, CSI s/u)
static int saved_cursor_row = 0, saved_cursor_col = 0;

// Cursor visibility
static bool cursor_visible = true;
static bool term_snap_cursor_visible = true;

// UTF-8 parsing state
static int utf8_remaining = 0;
static uint32_t utf8_codepoint = 0;

// --- SSH State ---

static ssh_session  ssh_sess    = NULL;
static ssh_channel  ssh_chan     = NULL;
static volatile bool ssh_connected = false;
static TaskHandle_t ssh_recv_task_handle = NULL;
static char ssh_last_host[64] = "";

// --- WiFi State ---

enum WifiState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED };
static volatile WifiState wifi_state = WIFI_IDLE;
#define WIFI_CONNECT_TIMEOUT_MS 15000
static char wifi_last_fail_ssid[64] = "";
static char wifi_last_fail_reason[64] = "";

// --- Helpers ---

struct LayoutInfo {
    int total_lines;
    int cursor_line;
    int cursor_col;
};

LayoutInfo computeLayoutFrom(const char* buf, int len, int cpos) {
    LayoutInfo info = {0, 0, 0};
    int line = 0, col = 0;

    for (int i = 0; i < len; i++) {
        if (i == cpos) {
            info.cursor_line = line;
            info.cursor_col  = col;
        }
        if (buf[i] == '\n') {
            line++; col = 0;
        } else {
            col++;
            if (col >= COLS_PER_LINE) { line++; col = 0; }
        }
    }
    if (cpos == len) {
        info.cursor_line = line;
        info.cursor_col  = col;
    }
    info.total_lines = line + 1;
    return info;
}

// --- Terminal Buffer Operations ---

void terminalClear() {
    for (int i = 0; i < TERM_ROWS; i++) {
        memset(term_buf[i], ' ', TERM_COLS);
        term_buf[i][TERM_COLS] = '\0';
    }
    term_line_count = 1;
    term_scroll     = 0;
    term_cursor_row = 0;
    term_cursor_col = 0;
    in_escape  = false;
    in_bracket = false;
    csi_param_count = 0;
    csi_parsing_num = false;
    csi_private = false;
    scroll_region_top = 0;
    scroll_region_bot = ROWS_PER_SCREEN - 1;
    scroll_region_set = false;
    term_alt_active = false;
    cursor_visible = true;
    utf8_remaining = 0;
    utf8_codepoint = 0;
    saved_cursor_row = 0;
    saved_cursor_col = 0;
}

void terminalScrollRegionUp(int top, int bot) {
    if (top < 0) top = 0;
    if (bot >= TERM_ROWS) bot = TERM_ROWS - 1;
    for (int i = top; i < bot; i++) {
        memcpy(term_buf[i], term_buf[i + 1], TERM_COLS + 1);
    }
    memset(term_buf[bot], ' ', TERM_COLS);
    term_buf[bot][TERM_COLS] = '\0';
}

void terminalScrollRegionDown(int top, int bot) {
    if (top < 0) top = 0;
    if (bot >= TERM_ROWS) bot = TERM_ROWS - 1;
    for (int i = bot; i > top; i--) {
        memcpy(term_buf[i], term_buf[i - 1], TERM_COLS + 1);
    }
    memset(term_buf[top], ' ', TERM_COLS);
    term_buf[top][TERM_COLS] = '\0';
}

// Map Unicode codepoint to printable ASCII (box drawing, symbols)
char unicodeToAscii(uint32_t cp) {
    // Box drawing: U+2500-U+257F
    if (cp >= 0x2500 && cp <= 0x257F) {
        // Horizontal lines
        if (cp == 0x2500 || cp == 0x2501 || cp == 0x2504 || cp == 0x2505 ||
            cp == 0x2508 || cp == 0x2509 || cp == 0x254C || cp == 0x254D ||
            cp == 0x2550) return '-';
        // Vertical lines
        if (cp == 0x2502 || cp == 0x2503 || cp == 0x2506 || cp == 0x2507 ||
            cp == 0x250A || cp == 0x250B || cp == 0x254E || cp == 0x254F ||
            cp == 0x2551) return '|';
        // Everything else (corners, tees, crosses)
        return '+';
    }
    // Block elements: U+2580-U+259F
    if (cp >= 0x2580 && cp <= 0x259F) return '#';
    // Common symbols
    if (cp == 0x2713 || cp == 0x2714) return '*';  // checkmarks
    if (cp == 0x2022 || cp == 0x25CF) return '*';  // bullets
    if (cp == 0x25CB || cp == 0x25A0 || cp == 0x25A1) return '*';  // circles/squares
    if (cp == 0x2192) return '>';  // right arrow
    if (cp == 0x2190) return '<';  // left arrow
    if (cp == 0x2191) return '^';  // up arrow
    if (cp == 0x2193) return 'v';  // down arrow
    if (cp == 0x2026) return '.';  // ellipsis
    if (cp == 0x2014 || cp == 0x2013) return '-';  // em/en dash
    if (cp == 0x2018 || cp == 0x2019) return '\''; // smart quotes
    if (cp == 0x201C || cp == 0x201D) return '"';   // smart double quotes
    return 0;  // unknown - skip
}

void enterAltScreen() {
    if (term_alt_active) return;
    // Save main buffer state
    for (int i = 0; i < TERM_ROWS; i++)
        memcpy(term_alt_buf[i], term_buf[i], TERM_COLS + 1);
    saved_main_cursor_row = term_cursor_row;
    saved_main_cursor_col = term_cursor_col;
    saved_main_line_count = term_line_count;
    saved_main_scroll = term_scroll;
    // Clear screen for alt buffer
    for (int i = 0; i < TERM_ROWS; i++) {
        memset(term_buf[i], ' ', TERM_COLS);
        term_buf[i][TERM_COLS] = '\0';
    }
    term_cursor_row = 0;
    term_cursor_col = 0;
    term_line_count = 1;
    term_scroll = 0;
    scroll_region_top = 0;
    scroll_region_bot = ROWS_PER_SCREEN - 1;
    scroll_region_set = false;
    term_alt_active = true;
}

void leaveAltScreen() {
    if (!term_alt_active) return;
    // Restore main buffer
    for (int i = 0; i < TERM_ROWS; i++)
        memcpy(term_buf[i], term_alt_buf[i], TERM_COLS + 1);
    term_cursor_row = saved_main_cursor_row;
    term_cursor_col = saved_main_cursor_col;
    term_line_count = saved_main_line_count;
    term_scroll = saved_main_scroll;
    scroll_region_top = 0;
    scroll_region_bot = ROWS_PER_SCREEN - 1;
    scroll_region_set = false;
    term_alt_active = false;
}

void handleCSI(char final_char) {
    int p0 = (csi_param_count > 0) ? csi_params[0] : 0;
    int p1 = (csi_param_count > 1) ? csi_params[1] : 0;
    int max_row = term_alt_active ? ROWS_PER_SCREEN : TERM_ROWS;

    // Handle private mode sequences (CSI ? ...)
    if (csi_private) {
        if (final_char == 'h') {
            // Set mode
            for (int pi = 0; pi < csi_param_count; pi++) {
                switch (csi_params[pi]) {
                    case 1049: // Alt screen + save cursor
                        saved_cursor_row = term_cursor_row;
                        saved_cursor_col = term_cursor_col;
                        enterAltScreen();
                        break;
                    case 47:   // Alt screen (no cursor save)
                    case 1047:
                        enterAltScreen();
                        break;
                    case 25:   // Show cursor
                        cursor_visible = true;
                        break;
                }
            }
        } else if (final_char == 'l') {
            // Reset mode
            for (int pi = 0; pi < csi_param_count; pi++) {
                switch (csi_params[pi]) {
                    case 1049: // Leave alt screen + restore cursor
                        leaveAltScreen();
                        term_cursor_row = saved_cursor_row;
                        term_cursor_col = saved_cursor_col;
                        break;
                    case 47:
                    case 1047:
                        leaveAltScreen();
                        break;
                    case 25:   // Hide cursor
                        cursor_visible = false;
                        break;
                }
            }
        }
        return;
    }

    switch (final_char) {
        case 'A': // Cursor Up
            term_cursor_row -= (p0 > 0) ? p0 : 1;
            if (term_cursor_row < 0) term_cursor_row = 0;
            break;
        case 'B': // Cursor Down
            term_cursor_row += (p0 > 0) ? p0 : 1;
            if (term_cursor_row >= max_row) term_cursor_row = max_row - 1;
            break;
        case 'C': // Cursor Right
            term_cursor_col += (p0 > 0) ? p0 : 1;
            if (term_cursor_col >= TERM_COLS) term_cursor_col = TERM_COLS - 1;
            break;
        case 'D': // Cursor Left
            term_cursor_col -= (p0 > 0) ? p0 : 1;
            if (term_cursor_col < 0) term_cursor_col = 0;
            break;
        case 'E': // Cursor Next Line
            term_cursor_col = 0;
            term_cursor_row += (p0 > 0) ? p0 : 1;
            if (term_cursor_row >= max_row) term_cursor_row = max_row - 1;
            break;
        case 'F': // Cursor Previous Line
            term_cursor_col = 0;
            term_cursor_row -= (p0 > 0) ? p0 : 1;
            if (term_cursor_row < 0) term_cursor_row = 0;
            break;
        case 'H': // Cursor Position (row;col) — 1-based
        case 'f': // Same as H
            term_cursor_row = (p0 > 0) ? p0 - 1 : 0;
            term_cursor_col = (p1 > 0) ? p1 - 1 : 0;
            if (term_cursor_row >= max_row) term_cursor_row = max_row - 1;
            if (term_cursor_col >= TERM_COLS) term_cursor_col = TERM_COLS - 1;
            if (term_cursor_row >= term_line_count) term_line_count = term_cursor_row + 1;
            break;
        case 'J': // Erase in Display
            if (p0 == 0) {
                memset(&term_buf[term_cursor_row][term_cursor_col], ' ',
                       TERM_COLS - term_cursor_col);
                for (int r = term_cursor_row + 1; r < max_row; r++) {
                    memset(term_buf[r], ' ', TERM_COLS);
                }
            } else if (p0 == 1) {
                for (int r = 0; r < term_cursor_row; r++) {
                    memset(term_buf[r], ' ', TERM_COLS);
                }
                memset(term_buf[term_cursor_row], ' ', term_cursor_col + 1);
            } else if (p0 == 2 || p0 == 3) {
                for (int r = 0; r < max_row; r++) {
                    memset(term_buf[r], ' ', TERM_COLS);
                }
                term_cursor_row = 0;
                term_cursor_col = 0;
                term_line_count = 1;
                term_scroll = 0;
            }
            break;
        case 'K': // Erase in Line
            if (p0 == 0) {
                memset(&term_buf[term_cursor_row][term_cursor_col], ' ',
                       TERM_COLS - term_cursor_col);
            } else if (p0 == 1) {
                memset(term_buf[term_cursor_row], ' ', term_cursor_col + 1);
            } else if (p0 == 2) {
                memset(term_buf[term_cursor_row], ' ', TERM_COLS);
            }
            break;
        case 'G': // Cursor Horizontal Absolute
            term_cursor_col = (p0 > 0) ? p0 - 1 : 0;
            if (term_cursor_col >= TERM_COLS) term_cursor_col = TERM_COLS - 1;
            break;
        case 'd': // Cursor Vertical Absolute
            term_cursor_row = (p0 > 0) ? p0 - 1 : 0;
            if (term_cursor_row >= max_row) term_cursor_row = max_row - 1;
            if (term_cursor_row >= term_line_count) term_line_count = term_cursor_row + 1;
            break;
        case 'P': { // Delete Characters
            int n = (p0 > 0) ? p0 : 1;
            int r = term_cursor_row;
            int c = term_cursor_col;
            if (c + n > TERM_COLS) n = TERM_COLS - c;
            memmove(&term_buf[r][c], &term_buf[r][c + n], TERM_COLS - c - n);
            memset(&term_buf[r][TERM_COLS - n], ' ', n);
            break;
        }
        case '@': { // Insert Characters
            int n = (p0 > 0) ? p0 : 1;
            int r = term_cursor_row;
            int c = term_cursor_col;
            if (c + n > TERM_COLS) n = TERM_COLS - c;
            memmove(&term_buf[r][c + n], &term_buf[r][c], TERM_COLS - c - n);
            memset(&term_buf[r][c], ' ', n);
            break;
        }
        case 'X': { // Erase Characters (overwrite with spaces, don't move cursor)
            int n = (p0 > 0) ? p0 : 1;
            int c = term_cursor_col;
            if (c + n > TERM_COLS) n = TERM_COLS - c;
            memset(&term_buf[term_cursor_row][c], ' ', n);
            break;
        }
        case 'L': { // Insert Lines (within scroll region)
            int n = (p0 > 0) ? p0 : 1;
            int bot = (term_alt_active || scroll_region_set) ? scroll_region_bot : max_row - 1;
            for (int j = bot; j >= term_cursor_row + n; j--) {
                memcpy(term_buf[j], term_buf[j - n], TERM_COLS + 1);
            }
            for (int j = term_cursor_row; j < term_cursor_row + n && j <= bot; j++) {
                memset(term_buf[j], ' ', TERM_COLS);
                term_buf[j][TERM_COLS] = '\0';
            }
            break;
        }
        case 'M': { // Delete Lines (within scroll region)
            int n = (p0 > 0) ? p0 : 1;
            int bot = (term_alt_active || scroll_region_set) ? scroll_region_bot : max_row - 1;
            for (int j = term_cursor_row; j + n <= bot; j++) {
                memcpy(term_buf[j], term_buf[j + n], TERM_COLS + 1);
            }
            for (int j = bot - n + 1; j <= bot; j++) {
                if (j >= 0) {
                    memset(term_buf[j], ' ', TERM_COLS);
                    term_buf[j][TERM_COLS] = '\0';
                }
            }
            break;
        }
        case 'S': { // Scroll Up (within scroll region)
            int n = (p0 > 0) ? p0 : 1;
            for (int j = 0; j < n; j++)
                terminalScrollRegionUp(scroll_region_top, scroll_region_bot);
            break;
        }
        case 'T': { // Scroll Down (within scroll region)
            int n = (p0 > 0) ? p0 : 1;
            for (int j = 0; j < n; j++)
                terminalScrollRegionDown(scroll_region_top, scroll_region_bot);
            break;
        }
        case 'r': { // Set Scroll Region (top;bottom, 1-based)
            if (p0 == 0 && p1 == 0) {
                // Reset scroll region
                scroll_region_top = 0;
                scroll_region_bot = ROWS_PER_SCREEN - 1;
                scroll_region_set = false;
            } else {
                scroll_region_top = (p0 > 0) ? p0 - 1 : 0;
                scroll_region_bot = (p1 > 0) ? p1 - 1 : ROWS_PER_SCREEN - 1;
                if (scroll_region_top < 0) scroll_region_top = 0;
                if (scroll_region_bot >= ROWS_PER_SCREEN) scroll_region_bot = ROWS_PER_SCREEN - 1;
                if (scroll_region_top >= scroll_region_bot) {
                    scroll_region_top = 0;
                    scroll_region_bot = ROWS_PER_SCREEN - 1;
                }
                scroll_region_set = true;
            }
            term_cursor_row = 0;
            term_cursor_col = 0;
            break;
        }
        case 's': // Save cursor position
            saved_cursor_row = term_cursor_row;
            saved_cursor_col = term_cursor_col;
            break;
        case 'u': // Restore cursor position
            term_cursor_row = saved_cursor_row;
            term_cursor_col = saved_cursor_col;
            break;
        case 'h': // Set mode (non-private)
        case 'l': // Reset mode (non-private)
            break;  // ignore standard modes
        default:
            break;
    }
}

// Handle cursor moving past bottom of scroll region or screen
void terminalCursorDown() {
    if (term_alt_active || scroll_region_set) {
        // Screen-bounded mode: scroll region
        if (term_cursor_row == scroll_region_bot) {
            terminalScrollRegionUp(scroll_region_top, scroll_region_bot);
        } else if (term_cursor_row < scroll_region_bot) {
            term_cursor_row++;
        }
    } else {
        // Main screen: scrollback mode
        term_cursor_row++;
        if (term_cursor_row >= TERM_ROWS) {
            terminalScrollRegionUp(0, TERM_ROWS - 1);
            term_cursor_row = TERM_ROWS - 1;
        }
        if (term_cursor_row >= term_line_count) {
            term_line_count = term_cursor_row + 1;
        }
    }
}

void terminalPutChar(char ch) {
    term_buf[term_cursor_row][term_cursor_col] = ch;
    term_cursor_col++;
    if (term_cursor_col >= TERM_COLS) {
        term_cursor_col = 0;
        terminalCursorDown();
    }
}

void terminalAppendOutput(const char* data, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];

        // UTF-8 multi-byte sequence continuation
        if (utf8_remaining > 0) {
            if ((c & 0xC0) == 0x80) {
                utf8_codepoint = (utf8_codepoint << 6) | (c & 0x3F);
                utf8_remaining--;
                if (utf8_remaining == 0) {
                    char mapped = unicodeToAscii(utf8_codepoint);
                    if (mapped) terminalPutChar(mapped);
                    // else skip unknown/zero-width characters
                }
            } else {
                // Invalid continuation - reset and reprocess
                utf8_remaining = 0;
                i--; // reprocess this byte
            }
            continue;
        }

        // ANSI escape sequence handling
        if (in_escape) {
            if (!in_bracket) {
                if (c == '[') {
                    in_bracket = true;
                    csi_param_count = 0;
                    csi_parsing_num = false;
                    csi_private = false;
                    memset(csi_params, 0, sizeof(csi_params));
                    continue;
                }
                if (c == ']') {
                    // OSC sequence — skip until ST (ESC \ or BEL)
                    in_escape = false;
                    in_bracket = false;
                    while (i + 1 < len) {
                        i++;
                        if ((unsigned char)data[i] == 0x07) break;
                        if ((unsigned char)data[i] == 0x1B && i + 1 < len && data[i+1] == '\\') {
                            i++;
                            break;
                        }
                    }
                    continue;
                }
                // Single char after ESC
                in_escape = false;
                switch (c) {
                    case 'M': // Reverse Index — cursor up, scroll region down if at top
                        if (term_cursor_row == scroll_region_top) {
                            terminalScrollRegionDown(scroll_region_top, scroll_region_bot);
                        } else if (term_cursor_row > 0) {
                            term_cursor_row--;
                        }
                        break;
                    case 'D': // Index — cursor down, scroll region up if at bottom
                        if (term_cursor_row == scroll_region_bot) {
                            terminalScrollRegionUp(scroll_region_top, scroll_region_bot);
                        } else {
                            terminalCursorDown();
                        }
                        break;
                    case 'E': // Next Line — cursor to start of next line
                        term_cursor_col = 0;
                        terminalCursorDown();
                        break;
                    case '7': // Save cursor
                        saved_cursor_row = term_cursor_row;
                        saved_cursor_col = term_cursor_col;
                        break;
                    case '8': // Restore cursor
                        term_cursor_row = saved_cursor_row;
                        term_cursor_col = saved_cursor_col;
                        break;
                    case 'c': // Full reset
                        terminalClear();
                        break;
                    case '(': case ')': case '*': case '+':
                        // Character set designation — skip next byte
                        if (i + 1 < len) i++;
                        break;
                    default:
                        break; // ignore unknown ESC sequences
                }
                continue;
            }
            // Inside ESC[ ... collecting parameters
            if (c >= '0' && c <= '9') {
                if (!csi_parsing_num) {
                    if (csi_param_count < MAX_CSI_PARAMS) {
                        csi_params[csi_param_count] = 0;
                        csi_parsing_num = true;
                    }
                }
                if (csi_param_count < MAX_CSI_PARAMS) {
                    csi_params[csi_param_count] = csi_params[csi_param_count] * 10 + (c - '0');
                }
                continue;
            }
            if (c == ';') {
                if (csi_parsing_num) {
                    csi_param_count++;
                    csi_parsing_num = false;
                } else {
                    if (csi_param_count < MAX_CSI_PARAMS) {
                        csi_params[csi_param_count] = 0;
                    }
                    csi_param_count++;
                }
                continue;
            }
            if (c == '?') {
                csi_private = true;
                continue;
            }
            if (c == '>' || c == '!' || c == ' ') {
                // Other prefixes/intermediates — continue collecting
                continue;
            }
            // Final character — execute CSI sequence
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '@' || c == '`') {
                if (csi_parsing_num && csi_param_count < MAX_CSI_PARAMS) {
                    csi_param_count++;
                }
                handleCSI((char)c);
                in_escape  = false;
                in_bracket = false;
            }
            continue;
        }

        if (c == 0x1B) { // ESC
            in_escape  = true;
            in_bracket = false;
            continue;
        }

        // UTF-8 start bytes
        if ((c & 0xE0) == 0xC0) { // 2-byte sequence
            utf8_codepoint = c & 0x1F;
            utf8_remaining = 1;
            continue;
        }
        if ((c & 0xF0) == 0xE0) { // 3-byte sequence
            utf8_codepoint = c & 0x0F;
            utf8_remaining = 2;
            continue;
        }
        if ((c & 0xF8) == 0xF0) { // 4-byte sequence
            utf8_codepoint = c & 0x07;
            utf8_remaining = 3;
            continue;
        }

        if (c == '\n') {
            term_cursor_col = 0;
            terminalCursorDown();
            continue;
        }

        if (c == '\r') {
            term_cursor_col = 0;
            continue;
        }

        if (c == '\b' || c == 0x7F) {
            if (term_cursor_col > 0) {
                term_cursor_col--;
                term_buf[term_cursor_row][term_cursor_col] = ' ';
            }
            continue;
        }

        if (c == '\t') {
            int next_tab = (term_cursor_col + 8) & ~7;
            if (next_tab > TERM_COLS) next_tab = TERM_COLS;
            while (term_cursor_col < next_tab) {
                term_buf[term_cursor_row][term_cursor_col] = ' ';
                term_cursor_col++;
            }
            if (term_cursor_col >= TERM_COLS) {
                term_cursor_col = 0;
                terminalCursorDown();
            }
            continue;
        }

        // Printable ASCII characters
        if (c >= ' ' && c <= '~') {
            terminalPutChar((char)c);
            continue;
        }
        // Non-printable / stray continuation bytes: ignore
    }

    // Auto-scroll to keep cursor visible
    if (term_cursor_row >= term_scroll + ROWS_PER_SCREEN) {
        term_scroll = term_cursor_row - ROWS_PER_SCREEN + 1;
    }
    if (term_cursor_row < term_scroll) {
        term_scroll = term_cursor_row;
    }
}

// --- SD Card ---

void sdAcquire() {
    sd_busy = true;
    while (!display_idle) vTaskDelay(1);
}
void sdRelease() { sd_busy = false; }

void sdInit() {
    if (SD.begin(BOARD_SD_CS, SPI, 4000000)) {
        sd_mounted = true;
        Serial.printf("SD: mounted, size=%lluMB\n", SD.cardSize() / (1024 * 1024));
    } else {
        sd_mounted = false;
        Serial.println("SD: mount failed");
    }
}

void sdLoadConfig() {
    if (!sd_mounted) return;
    File f = SD.open("/CONFIG", FILE_READ);
    if (!f) { Serial.println("SD: no /CONFIG"); return; }

    // CONFIG format: section-based, # comments, blank lines skipped
    // # wifi — pairs of ssid/password (variable count)
    // # ssh — host, port, user, pass, [optional vpn-host]
    // # vpn — ENABLE, privkey, pubkey, psk, ip, endpoint, port, [optional dns]
    // # bt  — optional BLE HID settings (name, [optional 6-digit pin])
    enum { SEC_WIFI, SEC_SSH, SEC_VPN, SEC_BT } section = SEC_WIFI;
    int field = 0;  // field index within current section
    bool wifi_expect_ssid = true;
    char wifi_ssid[64] = "";

    auto addWiFiAP = [&](const char* ssid, const char* pass) {
        if (config_wifi_count >= MAX_WIFI_APS) return;
        strncpy(config_wifi[config_wifi_count].ssid, ssid, 63);
        config_wifi[config_wifi_count].ssid[63] = '\0';
        strncpy(config_wifi[config_wifi_count].pass, pass, 63);
        config_wifi[config_wifi_count].pass[63] = '\0';
        Serial.printf("SD: WiFi AP added: %s%s\n",
                      config_wifi[config_wifi_count].ssid,
                      config_wifi[config_wifi_count].pass[0] ? "" : " (open)");
        config_wifi_count++;
    };

    auto flushPendingOpenWiFi = [&]() {
        if (section == SEC_WIFI && !wifi_expect_ssid) {
            addWiFiAP(wifi_ssid, "");
            wifi_expect_ssid = true;
            wifi_ssid[0] = '\0';
        }
    };

    auto parseBtPasskey = [&](const String& value, uint32_t* out) -> bool {
        if (!out) return false;
        if (value.length() != 6) return false;
        uint32_t pin = 0;
        for (size_t i = 0; i < 6; i++) {
            char c = value.charAt(i);
            if (c < '0' || c > '9') return false;
            pin = pin * 10 + (uint32_t)(c - '0');
        }
        if (pin < 100000 || pin > 999999) return false;
        *out = pin;
        return true;
    };

    auto setBtName = [&](const String& value) {
        if (value.length() == 0) return;
        strncpy(config_bt_name, value.c_str(), sizeof(config_bt_name) - 1);
        config_bt_name[sizeof(config_bt_name) - 1] = '\0';
    };

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) {
            // In WiFi section, blank password means open network.
            flushPendingOpenWiFi();
            continue;
        }
        if (line[0] == '#') {
            // Section headers
            flushPendingOpenWiFi();
            if (line.indexOf("wifi") >= 0)     { section = SEC_WIFI; field = 0; wifi_expect_ssid = true; wifi_ssid[0] = '\0'; }
            else if (line.indexOf("ssh") >= 0) { section = SEC_SSH; field = 0; }
            else if (line.indexOf("vpn") >= 0) { section = SEC_VPN; field = 0; }
            else if (line.indexOf("bt") >= 0)  {
                section = SEC_BT;
                field = 0;
                // Section presence enables BLE.
                config_bt_enabled = true;
            }
            continue;
        }

        if (section == SEC_WIFI) {
            if (wifi_expect_ssid) {
                strncpy(wifi_ssid, line.c_str(), 63);
                wifi_ssid[63] = '\0';
                wifi_expect_ssid = false;
            } else {
                addWiFiAP(wifi_ssid, line.c_str());
                wifi_expect_ssid = true;
                wifi_ssid[0] = '\0';
            }
        } else if (section == SEC_SSH) {
            switch (field) {
                case 0:
                    strncpy(config_ssh_host, line.c_str(), 63);
                    config_ssh_host[63] = '\0';
                    break;
                case 1: config_ssh_port = line.toInt(); break;
                case 2:
                    strncpy(config_ssh_user, line.c_str(), 63);
                    config_ssh_user[63] = '\0';
                    break;
                case 3:
                    strncpy(config_ssh_pass, line.c_str(), 63);
                    config_ssh_pass[63] = '\0';
                    break;
                case 4:
                    strncpy(config_ssh_vpn_host, line.c_str(), 63);
                    config_ssh_vpn_host[63] = '\0';
                    break;
            }
            field++;
        } else if (section == SEC_VPN) {
            // Backward compatibility: allow optional "ENABLE" line before VPN fields.
            if (field == 0) {
                String lowered = line;
                lowered.toLowerCase();
                if (lowered == "enable" || lowered == "enabled" || lowered == "true" || lowered == "1") {
                    continue;
                }
            }
            switch (field) {
                case 0:
                    strncpy(config_vpn_privkey, line.c_str(), 63);
                    config_vpn_privkey[63] = '\0';
                    break;
                case 1:
                    strncpy(config_vpn_pubkey, line.c_str(), 63);
                    config_vpn_pubkey[63] = '\0';
                    break;
                case 2:
                    strncpy(config_vpn_psk, line.c_str(), 63);
                    config_vpn_psk[63] = '\0';
                    break;
                case 3:
                    strncpy(config_vpn_ip, line.c_str(), 31);
                    config_vpn_ip[31] = '\0';
                    break;
                case 4:
                    strncpy(config_vpn_endpoint, line.c_str(), 63);
                    config_vpn_endpoint[63] = '\0';
                    break;
                case 5: config_vpn_port = line.toInt(); break;
                case 6:
                    strncpy(config_vpn_dns, line.c_str(), 31);
                    config_vpn_dns[31] = '\0';
                    break;
            }
            field++;
        } else if (section == SEC_BT) {
            // Strict positional parsing:
            // line 1: device name
            // line 2 (optional): 6-digit passkey
            if (field == 0) {
                setBtName(line);
                field = 1;
                continue;
            } else if (field == 1) {
                uint32_t parsed = 0;
                if (parseBtPasskey(line, &parsed)) {
                    config_bt_passkey = parsed;
                } else {
                    Serial.printf("SD: BT pin ignored (need 6 digits): %s\n", line.c_str());
                }
                field++;
                continue;
            }
            field++;
        }
    }
    flushPendingOpenWiFi();
    f.close();
    Serial.printf("SD: config loaded (%d WiFi APs, host=%s, VPN=%s, BT=%s, BT name=%s)\n",
                  config_wifi_count,
                  config_ssh_host,
                  vpnConfigured() ? "yes" : "no",
                  config_bt_enabled ? "on" : "off",
                  config_bt_name);
}

// --- File I/O Helpers ---

struct FileEntry {
    char name[64];
    bool is_dir;
    size_t size;
};

#define MAX_FILE_LIST 50
static FileEntry file_list[MAX_FILE_LIST];
static int file_list_count = 0;
static bool cmd_edit_picker_active = false;
static int  cmd_edit_picker_indices[MAX_FILE_LIST];
static int  cmd_edit_picker_count = 0;
static int  cmd_edit_picker_selected = 0;
static int  cmd_edit_picker_top = 0;

int listDirectory(const char* path);
bool loadFromFile(const char* path);
void autoSaveDirty();

void cmdClearResult() {
    cmd_result_count = 0;
    cmd_result_valid = false;
    for (int i = 0; i < CMD_RESULT_LINES; i++) cmd_result[i][0] = '\0';
}

void cmdAddLine(const char* fmt, ...) {
    if (cmd_result_count >= CMD_RESULT_LINES) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(cmd_result[cmd_result_count], COLS_PER_LINE + 1, fmt, args);
    va_end(args);
    cmd_result_count++;
    cmd_result_valid = true;
}

void cmdSetResult(const char* fmt, ...) {
    cmdClearResult();
    va_list args;
    va_start(args, fmt);
    vsnprintf(cmd_result[0], COLS_PER_LINE + 1, fmt, args);
    va_end(args);
    cmd_result_count = 1;
    cmd_result_valid = true;
}

int cmdEditPickerVisibleRows() {
    int rows = CMD_RESULT_LINES - 1; // Reserve first line for picker status/help text
    if (rows < 1) rows = 1;
    return rows;
}

void cmdEditPickerStop() {
    cmd_edit_picker_active = false;
    cmd_edit_picker_count = 0;
    cmd_edit_picker_selected = 0;
    cmd_edit_picker_top = 0;
}

void cmdEditPickerSyncViewport() {
    if (cmd_edit_picker_count <= 0) {
        cmd_edit_picker_top = 0;
        cmd_edit_picker_selected = 0;
        return;
    }
    if (cmd_edit_picker_selected < 0) cmd_edit_picker_selected = 0;
    if (cmd_edit_picker_selected >= cmd_edit_picker_count) cmd_edit_picker_selected = cmd_edit_picker_count - 1;

    int rows = cmdEditPickerVisibleRows();
    if (cmd_edit_picker_selected < cmd_edit_picker_top) {
        cmd_edit_picker_top = cmd_edit_picker_selected;
    } else if (cmd_edit_picker_selected >= cmd_edit_picker_top + rows) {
        cmd_edit_picker_top = cmd_edit_picker_selected - rows + 1;
    }

    int max_top = cmd_edit_picker_count - rows;
    if (max_top < 0) max_top = 0;
    if (cmd_edit_picker_top < 0) cmd_edit_picker_top = 0;
    if (cmd_edit_picker_top > max_top) cmd_edit_picker_top = max_top;
}

void cmdEditPickerRender() {
    if (!cmd_edit_picker_active || cmd_edit_picker_count <= 0) {
        cmdSetResult("(no files)");
        return;
    }

    cmdEditPickerSyncViewport();
    cmdClearResult();
    cmdAddLine("Edit %d/%d W/S A/D Enter", cmd_edit_picker_selected + 1, cmd_edit_picker_count);

    int rows = cmdEditPickerVisibleRows();
    for (int i = 0; i < rows && cmd_result_count < CMD_RESULT_LINES; i++) {
        int list_idx = cmd_edit_picker_top + i;
        if (list_idx >= cmd_edit_picker_count) break;
        const FileEntry& entry = file_list[cmd_edit_picker_indices[list_idx]];
        cmdAddLine("%c%s %dB", (list_idx == cmd_edit_picker_selected) ? '>' : ' ', entry.name, (int)entry.size);
    }
}

bool cmdEditPickerStart() {
    int n = listDirectory("/");
    if (n < 0) {
        cmdEditPickerStop();
        cmdSetResult("Can't read SD");
        return false;
    }

    cmd_edit_picker_count = 0;
    for (int i = 0; i < n && i < MAX_FILE_LIST; i++) {
        if (!file_list[i].is_dir) {
            cmd_edit_picker_indices[cmd_edit_picker_count++] = i;
        }
    }

    if (cmd_edit_picker_count == 0) {
        cmdEditPickerStop();
        cmdSetResult("(no files)");
        return false;
    }

    cmd_edit_picker_active = true;
    cmd_edit_picker_selected = 0;
    cmd_edit_picker_top = 0;
    cmdEditPickerRender();
    return true;
}

bool cmdEditPickerMoveSelection(int delta) {
    if (!cmd_edit_picker_active || cmd_edit_picker_count <= 0) return false;
    int next = cmd_edit_picker_selected + delta;
    if (next < 0) next = 0;
    if (next >= cmd_edit_picker_count) next = cmd_edit_picker_count - 1;
    if (next == cmd_edit_picker_selected) return false;
    cmd_edit_picker_selected = next;
    cmdEditPickerRender();
    return true;
}

bool cmdEditPickerPage(int direction) {
    int rows = cmdEditPickerVisibleRows();
    if (rows < 1) rows = 1;
    return cmdEditPickerMoveSelection(direction * rows);
}

bool cmdEditPickerOpenSelected() {
    if (!cmd_edit_picker_active || cmd_edit_picker_count <= 0) return false;

    int list_idx = cmd_edit_picker_selected;
    if (list_idx < 0 || list_idx >= cmd_edit_picker_count) return false;
    const FileEntry& entry = file_list[cmd_edit_picker_indices[list_idx]];

    autoSaveDirty();
    String path = "/" + String(entry.name);
    bool ok = loadFromFile(path.c_str());
    cmdEditPickerStop();
    if (ok) {
        current_file = path;
        cmdSetResult("Loaded %s (%d B)", entry.name, text_len);
        app_mode = MODE_NOTEPAD;
    } else {
        cmdSetResult("Load failed: %s", entry.name);
    }
    return true;
}

int listDirectory(const char* path) {
    file_list_count = 0;
    sdAcquire();
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        sdRelease();
        return -1;
    }
    File entry = dir.openNextFile();
    while (entry && file_list_count < MAX_FILE_LIST) {
        const char* name = entry.name();
        // Get just the filename (skip path prefix)
        const char* slash = strrchr(name, '/');
        const char* display_name = slash ? slash + 1 : name;
        if (display_name[0] != '.') {
            strncpy(file_list[file_list_count].name, display_name, sizeof(file_list[0].name) - 1);
            file_list[file_list_count].name[sizeof(file_list[0].name) - 1] = '\0';
            file_list[file_list_count].is_dir = entry.isDirectory();
            file_list[file_list_count].size = entry.size();
            file_list_count++;
        }
        entry = dir.openNextFile();
    }
    dir.close();
    sdRelease();
    return file_list_count;
}

bool saveToFile(const char* path) {
    sdAcquire();
    File f = SD.open(path, FILE_WRITE);
    if (!f) { sdRelease(); return false; }
    f.write((const uint8_t*)text_buf, text_len);
    f.close();
    sdRelease();
    file_modified = false;
    return true;
}

bool loadFromFile(const char* path) {
    sdAcquire();
    File f = SD.open(path, FILE_READ);
    if (!f) { sdRelease(); return false; }
    size_t sz = f.size();
    if (sz > MAX_TEXT_LEN) sz = MAX_TEXT_LEN;
    text_len = f.read((uint8_t*)text_buf, sz);
    text_buf[text_len] = '\0';
    cursor_pos = text_len;
    scroll_line = 0;
    f.close();
    sdRelease();
    file_modified = false;
    return true;
}

void autoSaveDirty() {
    if (!file_modified || text_len == 0) return;
    if (current_file.length() == 0) current_file = "/UNSAVED";
    saveToFile(current_file.c_str());
}

#include "network_module.hpp"
#include "bluetooth_module.hpp"
#include "screen_module.hpp"
#include "keyboard_module.hpp"
#include "cli_module.hpp"

// --- Setup & Loop ---

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("T-Deck Pro Notepad + Terminal starting...");

    // Clear any GPIO deep-sleep holds from a prior power-off cycle.
    gpio_hold_dis((gpio_num_t)BOARD_LORA_EN);
    gpio_hold_dis((gpio_num_t)BOARD_GPS_EN);
    gpio_hold_dis((gpio_num_t)BOARD_1V8_EN);
    gpio_hold_dis((gpio_num_t)BOARD_KEYBOARD_LED);
    gpio_hold_dis((gpio_num_t)BOARD_LORA_CS);
    gpio_hold_dis((gpio_num_t)BOARD_LORA_RST);
    gpio_hold_dis((gpio_num_t)BOARD_SD_CS);
    gpio_hold_dis((gpio_num_t)BOARD_EPD_CS);
    gpio_deep_sleep_hold_dis();

    // BOOT is GPIO0 (active-low). Hold to trigger the same deep sleep path as "off".
    pinMode(BOARD_BOOT_PIN, INPUT_PULLUP);

    // Disable task watchdog — SSH blocking calls would trigger it
    esp_task_wdt_deinit();

    // Disable unused peripherals
    pinMode(BOARD_LORA_EN, OUTPUT);        digitalWrite(BOARD_LORA_EN, LOW);
    pinMode(BOARD_GPS_EN, OUTPUT);         digitalWrite(BOARD_GPS_EN, LOW);
    pinMode(BOARD_1V8_EN, OUTPUT);         digitalWrite(BOARD_1V8_EN, LOW);
    // Keyboard backlight off
    pinMode(BOARD_KEYBOARD_LED, OUTPUT);
    digitalWrite(BOARD_KEYBOARD_LED, LOW);

    // SPI CS lines high
    pinMode(BOARD_LORA_CS, OUTPUT);  digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_LORA_RST, OUTPUT); digitalWrite(BOARD_LORA_RST, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);    digitalWrite(BOARD_SD_CS, HIGH);
    pinMode(BOARD_EPD_CS, OUTPUT);   digitalWrite(BOARD_EPD_CS, HIGH);

    // Init I2C
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);

    // Init keyboard
    if (!keypad.begin(0x34, &Wire)) {
        Serial.println("ERROR: TCA8418 keyboard not found!");
    } else {
        keypad.matrix(KEYPAD_ROWS, KEYPAD_COLS);
        keypad.flush();
        Serial.println("Keyboard OK");
    }

    // Init SPI & e-paper
    SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    display.init(115200, true, 2, false);
    display.setRotation(0);

    // Boost SPI clock from default 4MHz to 20MHz for faster data transfer
    display.epd2.selectSPI(SPI, SPISettings(20000000, MSBFIRST, SPI_MODE0));

    memset(text_buf, 0, sizeof(text_buf));

    // Init SD card and load config (before display task to avoid SPI contention)
    sdInit();
    sdLoadConfig();
    btInit();

    // Init terminal buffer
    terminalClear();

    // Create mutex
    state_mutex = xSemaphoreCreateMutex();

    // Launch display task on core 0 (Arduino loop runs on core 1)
    xTaskCreatePinnedToCore(
        displayTask,    // function
        "display",      // name
        8192,           // stack size
        NULL,           // parameter
        1,              // priority
        NULL,           // task handle
        0               // core 0
    );

    Serial.println("Ready. Double-tap MIC toggles notepad/terminal. Single-tap MIC for commands.");
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
}

// Core 1: keyboard polling — never blocks on display
void loop() {
    // Press BOOT to request power-off (same behavior as the "off" command).
    if (!poweroff_requested) {
        unsigned long now = millis();
        bool boot_pressed = (digitalRead(BOARD_BOOT_PIN) == LOW);
        if (boot_pressed) {
            if (boot_pressed_since == 0) {
                boot_pressed_since = now;
            }
            if (!boot_sleep_latched && (now - boot_pressed_since >= BOOT_SLEEP_HOLD_MS)) {
                boot_sleep_latched = true;
                poweroff_requested = true;
            }
        } else {
            boot_pressed_since = 0;
            boot_sleep_latched = false;
        }
    }

    // Check WiFi status (lightweight cached state check)
    static unsigned long last_net_check = 0;
    if (millis() - last_net_check > 5000) {
        last_net_check = millis();
        wifiCheck();
        // Auto-connect SSH if we switched to terminal and need it
        if (app_mode == MODE_TERMINAL && !ssh_connected && !ssh_connecting) {
            sshConnectAsync();
        }
    }
    // Battery check less often (I2C transaction)
    static unsigned long last_batt_check = 0;
    if (millis() - last_batt_check > 30000) {
        last_batt_check = millis();
        updateBattery();
    }

    // BLE maintenance: auto-advertise and reconnect handling.
    static unsigned long last_bt_check = 0;
    if (millis() - last_bt_check > 250) {
        last_bt_check = millis();
        btPoll();
    }

    // MIC single-tap timeout → open command processor
    if (mic_last_press > 0 && (millis() - mic_last_press >= MIC_DOUBLE_TAP_MS)) {
        mic_last_press = 0;
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        cmd_return_mode = app_mode;
        cmd_len = 0;
        cmd_buf[0] = '\0';
        cmd_result_valid = false;
        cmdEditPickerStop();
        app_mode = MODE_COMMAND;
        xSemaphoreGive(state_mutex);
        render_requested = true;
    }

    while (keypad.available() > 0) {
        int ev = keypad.getEvent();
        if (!(ev & 0x80)) continue;  // skip release events

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        bool needs_render = false;
        AppMode mode = app_mode;
        if (mode == MODE_NOTEPAD) {
            needs_render = handleNotepadKeyPress(ev);
        } else if (mode == MODE_TERMINAL) {
            needs_render = handleTerminalKeyPress(ev);
        } else if (mode == MODE_KEYBOARD) {
            needs_render = handleKeyboardModeKeyPress(ev);
        } else if (mode == MODE_COMMAND) {
            needs_render = handleCommandKeyPress(ev);
        }
        xSemaphoreGive(state_mutex);

        if (needs_render) {
            // After command execution, mode may have changed
            AppMode cur = app_mode;
            if (cur == MODE_NOTEPAD) {
                render_requested = true;
            } else if (cur == MODE_TERMINAL) {
                term_render_requested = true;
            } else if (cur == MODE_KEYBOARD) {
                term_render_requested = true;
            } else if (cur == MODE_COMMAND) {
                render_requested = true;
            }
        }
    }
}
