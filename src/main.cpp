#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <GxEPD2_BW.h>
#include <Adafruit_TCA8418.h>
#include <WiFi.h>
#include <libssh/libssh.h>
#include <esp_task_wdt.h>
#include <esp_sleep.h>
#include <esp_netif.h>
#include <driver/uart.h>
#include <SD.h>
#include <FS.h>
#include <WireGuard-ESP32.h>

// --- WiFi / SSH Config (loaded from SD /CONFIG) ---

#define MAX_WIFI_APS 8
struct WiFiAP { char ssid[64]; char pass[64]; };
static WiFiAP config_wifi[MAX_WIFI_APS];
static int config_wifi_count = 0;
static char config_ssh_host[64]   = "";
static int  config_ssh_port       = 22;
static char config_ssh_user[64]   = "";
static char config_ssh_pass[64]   = "";

// --- VPN Config (loaded from SD /CONFIG) ---
static char   config_vpn_privkey[64] = "";
static char   config_vpn_pubkey[64]  = "";
static char   config_vpn_psk[64]     = "";
static char   config_vpn_ip[32]      = "";
static char   config_vpn_endpoint[64] = "";
static int    config_vpn_port        = 51820;
static WireGuard wg;
static bool   vpn_connected = false;
static bool vpnConfigured() { return config_vpn_privkey[0] != '\0'; }

// Forward declarations
void connectMsg(const char* fmt, ...);
void powerOff();
static int partial_count = 0;

// --- SD Card State ---
static bool sd_mounted = false;
static volatile bool sd_busy = false;      // display task yields when true
static volatile bool display_idle = true;  // false while display is doing SPI

// --- Pin Definitions ---

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
#define BOARD_SD_CS         48

#define BOARD_KEYBOARD_INT  15
#define BOARD_KEYBOARD_LED  42

#define BOARD_LORA_EN       46
#define BOARD_GPS_EN        39
#define BOARD_1V8_EN        38

// --- A7682E 4G Modem Pins ---
#define BOARD_MODEM_POWER_EN  41   // Was BOARD_6609_EN — enables modem power rail
#define BOARD_MODEM_PWRKEY    40   // Power on/off toggle pulse
#define BOARD_MODEM_RST       9    // Hardware reset
#define BOARD_MODEM_RXD       10   // Modem UART RX (ESP32 TX → Modem RX)
#define BOARD_MODEM_TXD       11   // Modem UART TX (Modem TX → ESP32 RX)
#define BOARD_MODEM_DTR       8    // Data Terminal Ready
#define BOARD_MODEM_RI        7    // Ring Indicator

#define MODEM_APN       ""         // Set your carrier APN here (empty = auto)
#define MODEM_BAUD      115200

#define SerialAT Serial1

// --- Display ---

GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT> display(
    GxEPD2_310_GDEQ031T10(BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST, BOARD_EPD_BUSY)
);

// --- Keyboard ---

Adafruit_TCA8418 keypad;

#define KEYPAD_ROWS 4
#define KEYPAD_COLS 10

#define IS_LSHIFT(r, c) ((r) == 3 && (c) == 9)
#define IS_RSHIFT(r, c) ((r) == 3 && (c) == 5)
#define IS_SHIFT(r, c)  (IS_LSHIFT(r, c) || IS_RSHIFT(r, c))
#define IS_SYM(r, c)    ((r) == 3 && (c) == 8)
#define IS_ALT(r, c)    ((r) == 2 && (c) == 0)
#define IS_MIC(r, c)    ((r) == 3 && (c) == 6)
#define IS_DEAD(r, c)   ((r) == 3 && (c) <= 4)

static const char keymap_lower[KEYPAD_ROWS][KEYPAD_COLS] = {
    { 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p' },
    { 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\b' },
    {  0,  'z', 'x', 'c', 'v', 'b', 'n', 'm', '\b', '\n' },
    {  0,   0,   0,   0,   0,   0,   0,  ' ',   0,   0 },
};

static const char keymap_upper[KEYPAD_ROWS][KEYPAD_COLS] = {
    { 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P' },
    { 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', '\b' },
    {  0,  'Z', 'X', 'C', 'V', 'B', 'N', 'M', '\b', '\n' },
    {  0,   0,   0,   0,   0,   0,   0,  ' ',   0,   0 },
};

static const char keymap_sym[KEYPAD_ROWS][KEYPAD_COLS] = {
    { '#', '1', '2', '3', '(', ')', '_', '-', '+', '@' },
    { '*', '4', '5', '6', '/', ':', ';', '\'', '"', '\b' },
    {  0,  '7', '8', '9', '?', '!', ',', '.', '$', '\n' },
    {  0,   0,   0,   0,   0,   0,   0,  ' ',   0,   0 },
};

// --- App Mode ---

enum AppMode { MODE_NOTEPAD, MODE_TERMINAL, MODE_COMMAND };
static volatile AppMode app_mode = MODE_NOTEPAD;

// --- Editor State (shared between cores, protected by mutex) ---

#define MAX_TEXT_LEN    4096
#define CHAR_W          6
#define CHAR_H          8
#define MARGIN_X        2
#define MARGIN_Y        2
#define SCREEN_W        240
#define SCREEN_H        320
#define STATUS_H        10
#define COLS_PER_LINE   ((SCREEN_W - MARGIN_X * 2) / CHAR_W)
#define ROWS_PER_SCREEN ((SCREEN_H - MARGIN_Y - STATUS_H) / CHAR_H)

// --- Command Processor State ---
#define CMD_BUF_LEN 64
static char cmd_buf[CMD_BUF_LEN + 1];
static int  cmd_len = 0;
static AppMode cmd_return_mode = MODE_NOTEPAD;  // Mode to return to after command

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

#define TERM_ROWS       100
#define TERM_COLS       COLS_PER_LINE

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

// --- WiFi State ---

enum WifiState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED };
static volatile WifiState wifi_state = WIFI_IDLE;

// --- Modem State ---

enum ModemState { MODEM_OFF, MODEM_POWERING_ON, MODEM_AT_OK, MODEM_REGISTERING, MODEM_REGISTERED, MODEM_PPP_UP, MODEM_FAILED };
static volatile ModemState modem_state = MODEM_OFF;
static volatile bool modem_task_running = false;

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
    // # ssh — host, port, user, pass
    // # vpn — ENABLE, privkey, pubkey, psk, ip, endpoint, port
    enum { SEC_WIFI, SEC_SSH, SEC_VPN } section = SEC_WIFI;
    int field = 0;  // field index within current section
    char wifi_ssid[64] = "";

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        if (line[0] == '#') {
            // Section headers
            if (line.indexOf("ssh") >= 0)      { section = SEC_SSH; field = 0; }
            else if (line.indexOf("vpn") >= 0) { section = SEC_VPN; field = 0; }
            continue;
        }

        if (section == SEC_WIFI) {
            if (field % 2 == 0) {
                strncpy(wifi_ssid, line.c_str(), 63);
                wifi_ssid[63] = '\0';
            } else if (config_wifi_count < MAX_WIFI_APS) {
                strncpy(config_wifi[config_wifi_count].ssid, wifi_ssid, 63);
                strncpy(config_wifi[config_wifi_count].pass, line.c_str(), 63);
                Serial.printf("SD: WiFi AP added: %s\n", wifi_ssid);
                config_wifi_count++;
            }
            field++;
        } else if (section == SEC_SSH) {
            switch (field) {
                case 0: strncpy(config_ssh_host, line.c_str(), 63); break;
                case 1: config_ssh_port = line.toInt(); break;
                case 2: strncpy(config_ssh_user, line.c_str(), 63); break;
                case 3: strncpy(config_ssh_pass, line.c_str(), 63); break;
            }
            field++;
        } else if (section == SEC_VPN) {
            switch (field) {
                case 0: strncpy(config_vpn_privkey, line.c_str(), 63); break;
                case 1: strncpy(config_vpn_pubkey, line.c_str(), 63); break;
                case 2: strncpy(config_vpn_psk, line.c_str(), 63); break;
                case 3: strncpy(config_vpn_ip, line.c_str(), 31); break;
                case 4: strncpy(config_vpn_endpoint, line.c_str(), 63); break;
                case 5: config_vpn_port = line.toInt(); break;
            }
            field++;
        }
    }
    f.close();
    Serial.printf("SD: config loaded (%d WiFi APs, host=%s, VPN=%s)\n",
                  config_wifi_count, config_ssh_host, vpnConfigured() ? "yes" : "no");
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

// --- WiFi ---

// Try connecting to a single AP, returns true if connected within timeout
bool wifiTryAP(const char* ssid, const char* pass, int timeout_ms) {
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    WiFi.begin(ssid, pass);
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        if (WiFi.status() == WL_CONNECTED) return true;
        vTaskDelay(pdMS_TO_TICKS(250));
        elapsed += 250;
    }
    return WiFi.status() == WL_CONNECTED;
}

void wifiConnect() {
    if (wifi_state == WIFI_CONNECTING || wifi_state == WIFI_CONNECTED) return;
    wifi_state = WIFI_CONNECTING;
    WiFi.mode(WIFI_STA);
}

void wifiCheck() {
    if (wifi_state == WIFI_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            wifi_state = WIFI_CONNECTED;
            Serial.printf("WiFi: connected to %s, IP=%s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        }
    } else if (wifi_state == WIFI_CONNECTED) {
        if (WiFi.status() != WL_CONNECTED) {
            wifi_state = WIFI_FAILED;
            Serial.println("WiFi: connection lost");
        }
    }
}

// --- 4G Modem ---

String modemSendAT(const char* cmd, uint32_t timeout = 2000) {
    while (SerialAT.available()) SerialAT.read();  // flush input
    SerialAT.println(cmd);
    uint32_t start = millis();
    String response = "";
    while (millis() - start < timeout) {
        while (SerialAT.available()) {
            response += (char)SerialAT.read();
        }
        if (response.indexOf("OK") >= 0 || response.indexOf("ERROR") >= 0)
            break;
        delay(10);
    }
    Serial.printf("AT> %s => %s\n", cmd, response.c_str());
    return response;
}

void modemPowerOn() {
    Serial.println("Modem: powering on...");
    // Enable power rail
    pinMode(BOARD_MODEM_POWER_EN, OUTPUT);
    digitalWrite(BOARD_MODEM_POWER_EN, HIGH);
    delay(100);

    // PWRKEY pulse (short = power ON)
    pinMode(BOARD_MODEM_PWRKEY, OUTPUT);
    digitalWrite(BOARD_MODEM_PWRKEY, LOW);
    delay(10);
    digitalWrite(BOARD_MODEM_PWRKEY, HIGH);
    delay(50);
    digitalWrite(BOARD_MODEM_PWRKEY, LOW);
    delay(10);
}

void modemPowerOff() {
    Serial.println("Modem: powering off...");
    // Long PWRKEY pulse (3s = power OFF)
    digitalWrite(BOARD_MODEM_PWRKEY, LOW);
    delay(10);
    digitalWrite(BOARD_MODEM_PWRKEY, HIGH);
    delay(3000);
    digitalWrite(BOARD_MODEM_PWRKEY, LOW);
    delay(10);

    // Cut power rail
    digitalWrite(BOARD_MODEM_POWER_EN, LOW);
    modem_state = MODEM_OFF;
}

bool modemWaitAT() {
    for (int i = 0; i < 15; i++) {
        String r = modemSendAT("AT", 1000);
        if (r.indexOf("OK") >= 0) return true;
        delay(500);
    }
    return false;
}

bool modemWaitNetwork(uint32_t timeout = 60000) {
    uint32_t start = millis();
    while (millis() - start < timeout) {
        String r = modemSendAT("AT+CEREG?", 2000);
        // +CEREG: 0,1 = registered home, 0,5 = registered roaming
        if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) return true;
        // Also check GSM registration
        r = modemSendAT("AT+CREG?", 2000);
        if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) return true;
        delay(2000);
    }
    return false;
}

// Get signal quality (returns RSSI value, 0-31 scale, 99=unknown)
int modemGetSignal() {
    String r = modemSendAT("AT+CSQ", 2000);
    int idx = r.indexOf("+CSQ: ");
    if (idx >= 0) {
        int rssi = r.substring(idx + 6).toInt();
        return rssi;
    }
    return 99;
}

void modemConnectTask(void* param) {
    modem_task_running = true;
    modem_state = MODEM_POWERING_ON;
    term_render_requested = true;

    // Initialize UART
    SerialAT.begin(MODEM_BAUD, SERIAL_8N1, BOARD_MODEM_TXD, BOARD_MODEM_RXD);
    delay(100);

    // Power on modem
    modemPowerOn();
    delay(3000);  // Wait for modem to boot

    // Wait for AT response
    if (!modemWaitAT()) {
        Serial.println("Modem: AT failed, no response");
        modem_state = MODEM_FAILED;
        term_render_requested = true;
        modem_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    modem_state = MODEM_AT_OK;
    term_render_requested = true;
    Serial.println("Modem: AT OK");

    // Basic configuration
    modemSendAT("ATE0");           // Disable echo
    modemSendAT("AT+CMEE=2");     // Verbose error messages
    modemSendAT("AT+CPIN?", 5000); // Check SIM

    // Set APN if configured
    if (strlen(MODEM_APN) > 0) {
        char apn_cmd[64];
        snprintf(apn_cmd, sizeof(apn_cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", MODEM_APN);
        modemSendAT(apn_cmd);
    }

    // Wait for network registration
    modem_state = MODEM_REGISTERING;
    term_render_requested = true;
    Serial.println("Modem: waiting for network...");

    if (!modemWaitNetwork(90000)) {
        Serial.println("Modem: network registration failed");
        int sig = modemGetSignal();
        Serial.printf("Modem: signal strength: %d/31\n", sig);
        modem_state = MODEM_FAILED;
        term_render_requested = true;
        modem_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    modem_state = MODEM_REGISTERED;
    term_render_requested = true;

    int sig = modemGetSignal();
    Serial.printf("Modem: registered! Signal: %d/31\n", sig);

    // Attach to GPRS
    modemSendAT("AT+CGATT=1", 10000);

    // Activate PDP context
    modemSendAT("AT+CGACT=1,1", 10000);

    // Check IP address
    String ip_resp = modemSendAT("AT+CGPADDR=1", 5000);
    Serial.printf("Modem: IP response: %s\n", ip_resp.c_str());

    // For now, we have cellular data active via the modem's internal TCP stack.
    // Full PPPoS integration requires esp_modem component (future work).
    // As an interim solution, we can use AT+CIPSTART for TCP connections.

    modem_state = MODEM_PPP_UP;
    term_render_requested = true;
    Serial.println("Modem: cellular data active");

    modem_task_running = false;
    vTaskDelete(NULL);
}

void modemStartAsync() {
    if (modem_task_running || modem_state == MODEM_PPP_UP) return;
    xTaskCreatePinnedToCore(
        modemConnectTask,
        "modem_conn",
        8192,
        NULL,
        1,
        NULL,
        1  // run on core 1
    );
}

void modemStop() {
    if (modem_state == MODEM_OFF) return;
    modemPowerOff();
    SerialAT.end();
    modem_state = MODEM_OFF;
    term_render_requested = true;
    Serial.println("Modem: stopped");
}

// --- SSH ---

void sshDisconnect() {
    if (ssh_recv_task_handle) {
        vTaskDelete(ssh_recv_task_handle);
        ssh_recv_task_handle = NULL;
    }
    ssh_connected = false;
    if (ssh_chan) {
        ssh_channel_close(ssh_chan);
        ssh_channel_free(ssh_chan);
        ssh_chan = NULL;
    }
    if (ssh_sess) {
        ssh_disconnect(ssh_sess);
        ssh_free(ssh_sess);
        ssh_sess = NULL;
    }
    Serial.println("SSH: disconnected");
}

void sshReceiveTask(void* param);
void renderCommandPrompt();

bool hasNetwork() {
    return (wifi_state == WIFI_CONNECTED) || (modem_state == MODEM_PPP_UP);
}

bool vpnConnect() {
    if (vpn_connected) return true;
    connectMsg("VPN: NTP sync...");
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    struct tm tm;
    int tries = 0;
    while (!getLocalTime(&tm) && tries++ < 10) delay(500);
    if (tries >= 10) {
        connectMsg("VPN: NTP failed");
        return false;
    }
    connectMsg("VPN: connecting...");
    IPAddress local_ip;
    local_ip.fromString(config_vpn_ip);
    const char* psk = config_vpn_psk[0] ? config_vpn_psk : NULL;
    wg.begin(local_ip, config_vpn_privkey, config_vpn_endpoint,
             config_vpn_pubkey, config_vpn_port, psk);
    vpn_connected = true;
    connectMsg("VPN: %s", config_vpn_ip);
    return true;
}

bool sshTryConnect() {
    Serial.printf("SSH: connecting to %s:%d...\n", config_ssh_host, config_ssh_port);

    ssh_sess = ssh_new();
    if (!ssh_sess) return false;

    ssh_options_set(ssh_sess, SSH_OPTIONS_HOST, config_ssh_host);
    int port = config_ssh_port;
    ssh_options_set(ssh_sess, SSH_OPTIONS_PORT, &port);
    ssh_options_set(ssh_sess, SSH_OPTIONS_USER, config_ssh_user);
    long timeout = 5;  // 5 second connect timeout
    ssh_options_set(ssh_sess, SSH_OPTIONS_TIMEOUT, &timeout);

    if (ssh_connect(ssh_sess) != SSH_OK) {
        Serial.printf("SSH: connect failed: %s\n", ssh_get_error(ssh_sess));
        ssh_free(ssh_sess);
        ssh_sess = NULL;
        return false;
    }

    if (ssh_userauth_password(ssh_sess, NULL, config_ssh_pass) != SSH_AUTH_SUCCESS) {
        Serial.printf("SSH: auth failed: %s\n", ssh_get_error(ssh_sess));
        ssh_disconnect(ssh_sess);
        ssh_free(ssh_sess);
        ssh_sess = NULL;
        return false;
    }
    return true;
}

bool sshConnect() {
    if (!hasNetwork()) {
        Serial.println("SSH: no network (WiFi or 4G)");
        return false;
    }
    if (ssh_connected) {
        Serial.println("SSH: already connected");
        return false;
    }

    // Clean up any previous session
    sshDisconnect();

    // Try direct SSH first
    connectMsg("SSH: %s:%d...", config_ssh_host, config_ssh_port);
    if (sshTryConnect()) {
        connectMsg("SSH: connected");
    } else if (vpnConfigured() && !vpn_connected) {
        // Direct failed, try VPN
        connectMsg("SSH: direct failed");
        if (!vpnConnect()) {
            connectMsg("SSH: VPN failed");
            return false;
        }
        connectMsg("SSH: %s (VPN)...", config_ssh_host);
        if (!sshTryConnect()) {
            connectMsg("SSH: failed via VPN");
            return false;
        }
        connectMsg("SSH: connected (VPN)");
    } else {
        connectMsg("SSH: failed");
        return false;
    }

    ssh_chan = ssh_channel_new(ssh_sess);
    if (!ssh_chan) {
        Serial.println("SSH: channel_new failed");
        ssh_disconnect(ssh_sess);
        ssh_free(ssh_sess);
        ssh_sess = NULL;

        return false;
    }

    if (ssh_channel_open_session(ssh_chan) != SSH_OK) {
        Serial.printf("SSH: channel open failed: %s\n", ssh_get_error(ssh_sess));
        ssh_channel_free(ssh_chan);
        ssh_chan = NULL;
        ssh_disconnect(ssh_sess);
        ssh_free(ssh_sess);
        ssh_sess = NULL;

        return false;
    }

    // Request PTY sized to our screen
    if (ssh_channel_request_pty_size(ssh_chan, "xterm", TERM_COLS, ROWS_PER_SCREEN) != SSH_OK) {
        Serial.printf("SSH: pty request failed: %s\n", ssh_get_error(ssh_sess));
        ssh_channel_close(ssh_chan);
        ssh_channel_free(ssh_chan);
        ssh_chan = NULL;
        ssh_disconnect(ssh_sess);
        ssh_free(ssh_sess);
        ssh_sess = NULL;

        return false;
    }

    if (ssh_channel_request_shell(ssh_chan) != SSH_OK) {
        Serial.printf("SSH: shell request failed: %s\n", ssh_get_error(ssh_sess));
        ssh_channel_close(ssh_chan);
        ssh_channel_free(ssh_chan);
        ssh_chan = NULL;
        ssh_disconnect(ssh_sess);
        ssh_free(ssh_sess);
        ssh_sess = NULL;

        return false;
    }

    ssh_connected = true;
    Serial.println("SSH: connected!");

    // Clear terminal buffer for fresh session
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    terminalClear();
    xSemaphoreGive(state_mutex);

    // Launch receive task on core 0
    xTaskCreatePinnedToCore(
        sshReceiveTask,
        "ssh_recv",
        16384,
        NULL,
        1,
        &ssh_recv_task_handle,
        0
    );

    return true;
}

static volatile bool ssh_connecting = false;
#define CONNECT_STATUS_LINES 16
static char connect_status[CONNECT_STATUS_LINES][COLS_PER_LINE + 1];
static volatile int connect_status_count = 0;

void connectMsg(const char* fmt, ...) {
    if (connect_status_count >= CONNECT_STATUS_LINES) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(connect_status[connect_status_count], COLS_PER_LINE + 1, fmt, args);
    va_end(args);
    connect_status_count++;
    term_render_requested = true;
}

void sshConnectTask(void* param) {
    connect_status_count = 0;

    // Connect WiFi
    if (wifi_state != WIFI_CONNECTED) {
        WiFi.mode(WIFI_STA);
        bool connected = false;

        // Try each known AP in config order
        for (int i = 0; i < config_wifi_count && !connected; i++) {
            connectMsg("WiFi: %s...", config_wifi[i].ssid);
            if (wifiTryAP(config_wifi[i].ssid, config_wifi[i].pass, 5000)) {
                connected = true;
            } else {
                connectMsg("  failed");
            }
        }

        // If all failed, scan and try best match
        if (!connected) {
            connectMsg("WiFi: scanning...");
            int n = WiFi.scanNetworks();
            if (n > 0) {
                for (int i = 0; i < n && i < 4; i++) {
                    connectMsg("  %s (%ddBm)", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
                }
                // Try known APs sorted by signal strength
                for (int rssi_thresh = 0; rssi_thresh > -100 && !connected; rssi_thresh -= 10) {
                    for (int i = 0; i < n && !connected; i++) {
                        if (WiFi.RSSI(i) < rssi_thresh - 10 || WiFi.RSSI(i) >= rssi_thresh) continue;
                        for (int j = 0; j < config_wifi_count; j++) {
                            if (WiFi.SSID(i) == config_wifi[j].ssid) {
                                connectMsg("WiFi: %s...", config_wifi[j].ssid);
                                WiFi.scanDelete();
                                if (wifiTryAP(config_wifi[j].ssid, config_wifi[j].pass, 5000)) {
                                    connected = true;
                                }
                                break;
                            }
                        }
                    }
                }
                if (!connected) WiFi.scanDelete();
            }
        }

        if (connected) {
            wifi_state = WIFI_CONNECTED;
            connectMsg("WiFi: %s (%s)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        } else {
            connectMsg("WiFi: all failed");
            ssh_connecting = false;
            vTaskDelete(NULL);
            return;
        }
    } else {
        connectMsg("WiFi: %s", WiFi.SSID().c_str());
    }

    bool ok = sshConnect();
    ssh_connecting = false;
    if (ok) {
        connect_status_count = 0;
        partial_count = 100;  // force full clean redraw
    }
    term_render_requested = true;
    vTaskDelete(NULL);
}

void sshConnectAsync() {
    if (ssh_connecting || ssh_connected) return;
    ssh_connecting = true;
    xTaskCreatePinnedToCore(
        sshConnectTask,
        "ssh_conn",
        16384,
        NULL,
        1,
        NULL,
        1  // run on core 1
    );
}

void sshSendKey(char c) {
    if (!ssh_connected || !ssh_chan) return;
    ssh_channel_write(ssh_chan, &c, 1);
}

void sshSendString(const char* s, int len) {
    if (!ssh_connected || !ssh_chan) return;
    ssh_channel_write(ssh_chan, s, len);
}

void sshReceiveTask(void* param) {
    char recv_buf[512];
    for (;;) {
        if (!ssh_connected || !ssh_chan) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int nbytes = ssh_channel_read_nonblocking(ssh_chan, recv_buf, sizeof(recv_buf), 0);
        if (nbytes > 0) {
            xSemaphoreTake(state_mutex, portMAX_DELAY);
            terminalAppendOutput(recv_buf, nbytes);
            // Drain loop: keep reading to accumulate data before rendering
            int total = nbytes;
            for (int drain = 0; drain < 10 && total < 2048; drain++) {
                nbytes = ssh_channel_read_nonblocking(ssh_chan, recv_buf, sizeof(recv_buf), 0);
                if (nbytes <= 0) break;
                terminalAppendOutput(recv_buf, nbytes);
                total += nbytes;
            }
            xSemaphoreGive(state_mutex);
            term_render_requested = true;
        } else if (nbytes == SSH_ERROR || ssh_channel_is_eof(ssh_chan)) {
            Serial.println("SSH: channel closed by remote");
            ssh_connected = false;
            term_render_requested = true;
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

// --- Display Rendering (runs on core 0 only, uses snap_ vars) ---

void drawLinesRange(int first_line, int last_line) {
    int text_line = 0, col = 0;
    int start_i = 0;
    bool found_start = false;

    // Fast-forward to snap_scroll line to avoid scanning entire buffer
    int target_line = snap_scroll + first_line;
    if (target_line > 0) {
        for (int i = 0; i < snap_len; i++) {
            if (text_line >= target_line) { start_i = i; found_start = true; break; }
            if (snap_buf[i] == '\n') { text_line++; col = 0; }
            else { col++; if (col >= COLS_PER_LINE) { text_line++; col = 0; } }
        }
        // Handle trailing empty line (cursor after a newline): we may only
        // reach target_line exactly after consuming the final character.
        if (!found_start) {
            if (text_line < target_line) return; // not enough lines
            start_i = snap_len;
        }
    }

    // Batch buffer for accumulating runs of chars on the same line
    char run_buf[COLS_PER_LINE + 1];
    int run_start_col = -1;
    int run_len = 0;
    int run_sl = -1;

    // Flush accumulated run to display
    auto flushRun = [&]() {
        if (run_len > 0) {
            run_buf[run_len] = '\0';
            display.setCursor(MARGIN_X + run_start_col * CHAR_W, MARGIN_Y + run_sl * CHAR_H + 1);
            display.print(run_buf);
            run_len = 0;
            run_start_col = -1;
        }
    };

    for (int i = start_i; i <= snap_len; i++) {
        int sl = text_line - snap_scroll;

        if (i == snap_cursor && sl >= first_line && sl <= last_line) {
            flushRun();
            int x = MARGIN_X + col * CHAR_W;
            int y = MARGIN_Y + sl * CHAR_H;
            display.fillRect(x, y, CHAR_W, CHAR_H, GxEPD_BLACK);
            if (i < snap_len && snap_buf[i] != '\n') {
                display.setTextColor(GxEPD_WHITE);
                display.setCursor(x, y + 1);
                display.print(snap_buf[i]);
                display.setTextColor(GxEPD_BLACK);
            }
        }

        if (i >= snap_len) break;
        char c = snap_buf[i];
        if (c == '\n') {
            flushRun();
            text_line++; col = 0; continue;
        }

        if (sl >= first_line && sl <= last_line && i != snap_cursor) {
            if (run_sl != sl || run_start_col + run_len != col) {
                flushRun();
                run_start_col = col;
                run_sl = sl;
            }
            run_buf[run_len++] = c;
        } else {
            flushRun();
        }

        col++;
        if (col >= COLS_PER_LINE) {
            flushRun();
            text_line++; col = 0;
        }

        if (text_line - snap_scroll > last_line && i != snap_cursor) break;
    }
    flushRun();
}

// Battery reading via BQ27220 fuel gauge on I2C (address 0x55)
#define BQ27220_ADDR 0x55
#define BQ27220_REG_SOC 0x2C  // StateOfCharge register
#define BQ27220_REG_VOLT 0x08 // Voltage register
static int battery_pct = -1;  // -1 = unknown

void updateBattery() {
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(BQ27220_REG_SOC);
    if (Wire.endTransmission(false) != 0) {
        // Fuel gauge not responding, fall back to ADC
        uint32_t raw = analogReadMilliVolts(4);
        float voltage = (raw * 2.0f) / 1000.0f + 0.32f;
        int pct = (int)((voltage - 3.3f) / (4.2f - 3.3f) * 100.0f);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        battery_pct = pct;
        return;
    }
    if (Wire.requestFrom((uint8_t)BQ27220_ADDR, (uint8_t)2) == 2) {
        uint8_t lo = Wire.read();
        uint8_t hi = Wire.read();
        int soc = (hi << 8) | lo;
        if (soc >= 0 && soc <= 100) battery_pct = soc;
    }
}

// MIC double-tap detection
static unsigned long mic_last_press = 0;
#define MIC_DOUBLE_TAP_MS 350

void drawStatusBar(const LayoutInfo& info) {
    int bar_y = SCREEN_H - STATUS_H;
    display.fillRect(0, bar_y, SCREEN_W, STATUS_H, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(NULL);
    display.setCursor(2, bar_y + 1);
    char status[60];
    char mods[16] = "";
    if (snap_shift) strcat(mods, "SH ");
    if (snap_sym)   strcat(mods, "SY ");
    if (snap_nav)   strcat(mods, "NV ");
    // Show filename if editing a file
    const char* fname = "";
    if (current_file.length() > 0) {
        const char* s = current_file.c_str();
        const char* sl = strrchr(s, '/');
        fname = sl ? sl + 1 : s;
    }
    snprintf(status, sizeof(status), "%s%s L%d C%d %s",
             fname, file_modified ? "*" : "",
             info.cursor_line + 1, info.cursor_col + 1,
             mods);
    // Append battery on right side if known
    display.print(status);
    if (battery_pct >= 0) {
        char batt[8];
        snprintf(batt, sizeof(batt), "%d%%", battery_pct);
        int bx = SCREEN_W - strlen(batt) * CHAR_W - 2;
        display.setCursor(bx, bar_y + 1);
        display.print(batt);
    }
}

// --- Terminal Rendering ---

void snapshotTerminalState() {
    term_snap_scroll = term_scroll;
    term_snap_crow   = term_cursor_row;
    term_snap_ccol   = term_cursor_col;
    term_snap_lines  = term_line_count;
    term_snap_cursor_visible = cursor_visible;
    // Only copy visible rows + 1 for safety
    int first = term_snap_scroll;
    int last = first + ROWS_PER_SCREEN;
    if (first < 0) first = 0;
    if (last > TERM_ROWS) last = TERM_ROWS;
    for (int i = first; i < last; i++) {
        memcpy(term_snap_buf[i], term_buf[i], TERM_COLS + 1);
    }
}

void drawTerminalLines(int first_line, int last_line) {
    char run_buf[TERM_COLS + 1];
    for (int sl = first_line; sl <= last_line && sl < ROWS_PER_SCREEN; sl++) {
        int buf_row = term_snap_scroll + sl;
        if (buf_row < 0 || buf_row >= TERM_ROWS) continue;
        bool is_cursor_row = term_snap_cursor_visible && (buf_row == term_snap_crow);
        int y = MARGIN_Y + sl * CHAR_H;

        // Find runs of non-space characters and print them in batches
        int c = 0;
        while (c < TERM_COLS) {
            // Skip spaces (unless cursor is here)
            if (term_snap_buf[buf_row][c] == ' ' && !(is_cursor_row && c == term_snap_ccol)) {
                c++;
                continue;
            }

            // Draw cursor cell specially
            if (is_cursor_row && c == term_snap_ccol) {
                int x = MARGIN_X + c * CHAR_W;
                display.fillRect(x, y, CHAR_W, CHAR_H, GxEPD_BLACK);
                if (term_snap_buf[buf_row][c] != ' ') {
                    display.setTextColor(GxEPD_WHITE);
                    display.setCursor(x, y + 1);
                    display.print(term_snap_buf[buf_row][c]);
                    display.setTextColor(GxEPD_BLACK);
                }
                c++;
                continue;
            }

            // Collect run of non-space chars (stop before cursor)
            int run_start = c;
            int run_len = 0;
            while (c < TERM_COLS && term_snap_buf[buf_row][c] != ' ' &&
                   !(is_cursor_row && c == term_snap_ccol)) {
                run_buf[run_len++] = term_snap_buf[buf_row][c];
                c++;
            }
            run_buf[run_len] = '\0';
            display.setCursor(MARGIN_X + run_start * CHAR_W, y + 1);
            display.print(run_buf);
        }
    }
}

void drawTerminalStatusBar() {
    int bar_y = SCREEN_H - STATUS_H;
    display.fillRect(0, bar_y, SCREEN_W, STATUS_H, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(NULL);
    display.setCursor(2, bar_y + 1);

    char status[60];
    // Build compact connection string
    if (ssh_connecting) {
        snprintf(status, sizeof(status), vpn_connected ? "VPN SSH..." : "SSH...");
    } else if (ssh_connected) {
        const char* net = vpn_connected ? "VPN" : (modem_state == MODEM_PPP_UP) ? "4G" : "WiFi";
        snprintf(status, sizeof(status), "%s %s@%s", net, config_ssh_user, config_ssh_host);
    } else if (modem_state == MODEM_PPP_UP) {
        snprintf(status, sizeof(status), "4G OK");
    } else if (modem_state >= MODEM_POWERING_ON && modem_state <= MODEM_REGISTERED) {
        snprintf(status, sizeof(status), "4G...");
    } else if (modem_state == MODEM_FAILED) {
        snprintf(status, sizeof(status), "4G fail");
    } else if (wifi_state == WIFI_CONNECTED) {
        snprintf(status, sizeof(status), "WiFi %s", WiFi.localIP().toString().c_str());
    } else if (wifi_state == WIFI_CONNECTING) {
        snprintf(status, sizeof(status), "WiFi...");
    } else {
        snprintf(status, sizeof(status), "No net");
    }
    display.print(status);
    // Battery on right side
    if (battery_pct >= 0) {
        char batt[8];
        snprintf(batt, sizeof(batt), "%d%%", battery_pct);
        int bx = SCREEN_W - strlen(batt) * CHAR_W - 2;
        display.setCursor(bx, bar_y + 1);
        display.print(batt);
    }
}

void renderConnectScreen() {
    partial_count++;
    display.setPartialWindow(0, 0, SCREEN_W, SCREEN_H);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(NULL);
        int y = MARGIN_Y + CHAR_H * 2;  // start a couple lines down
        for (int i = 0; i < connect_status_count && i < CONNECT_STATUS_LINES; i++) {
            display.setCursor(MARGIN_X, y);
            display.print(connect_status[i]);
            y += CHAR_H + 2;
        }
        drawTerminalStatusBar();
    } while (display.nextPage());
}

void renderTerminal() {
    int y_start = 0;
    int region_h = SCREEN_H;

    partial_count++;
    display.setPartialWindow(0, y_start, SCREEN_W, region_h);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(NULL);
        drawTerminalLines(0, ROWS_PER_SCREEN - 1);
        drawTerminalStatusBar();
    } while (display.nextPage());
}

void renderTerminalFullClean() {
    partial_count = 0;
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(NULL);
        drawTerminalLines(0, ROWS_PER_SCREEN - 1);
        drawTerminalStatusBar();
    } while (display.nextPage());
}

// --- Notepad Rendering ---

void refreshLines(int first_line, int last_line, const LayoutInfo& layout) {
    if (first_line < 0) first_line = 0;
    if (last_line >= ROWS_PER_SCREEN) last_line = ROWS_PER_SCREEN - 1;

    int y_start = MARGIN_Y + first_line * CHAR_H;
    int region_h = SCREEN_H - y_start;

    partial_count++;

    display.setPartialWindow(0, y_start, SCREEN_W, region_h);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(NULL);
        drawLinesRange(first_line, ROWS_PER_SCREEN - 1);
        drawStatusBar(layout);
    } while (display.nextPage());
}

void refreshAllPartial(const LayoutInfo& layout) {
    partial_count++;

    display.setPartialWindow(0, 0, SCREEN_W, SCREEN_H);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(NULL);
        drawLinesRange(0, ROWS_PER_SCREEN - 1);
        drawStatusBar(layout);
    } while (display.nextPage());
}

void refreshFullClean(const LayoutInfo& layout) {
    partial_count = 0;
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(NULL);
        drawLinesRange(0, ROWS_PER_SCREEN - 1);
        drawStatusBar(layout);
    } while (display.nextPage());
}

// Take a snapshot of shared state (called with mutex held)
void snapshotState() {
    memcpy(snap_buf, text_buf, text_len + 1);
    snap_len    = text_len;
    snap_cursor = cursor_pos;
    snap_scroll = scroll_line;
    snap_shift  = shift_held;
    snap_sym    = sym_mode;
    snap_alt    = alt_mode;
    snap_nav    = nav_mode;
}

// --- Display Task (Core 0) ---

static LayoutInfo prev_layout = {1, 0, 0};

void displayTask(void* param) {
    // Initial full refresh
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    snapshotState();
    xSemaphoreGive(state_mutex);
    display_idle = false;
    prev_layout = computeLayoutFrom(snap_buf, snap_len, snap_cursor);
    refreshFullClean(prev_layout);
    display_idle = true;

    AppMode last_mode = MODE_NOTEPAD;

    for (;;) {
        // Yield SPI bus to SD card operations
        if (sd_busy) { vTaskDelay(1); continue; }

        // Power off requested — render art and enter deep sleep
        if (poweroff_requested) {
            powerOff();  // never returns
        }

        AppMode cur_mode = app_mode;

        // Mode switch — full redraw
        if (cur_mode != last_mode) {
            last_mode = cur_mode;
            partial_count = 0;
            display_idle = false;
            if (cur_mode == MODE_TERMINAL) {
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                snapshotTerminalState();
                xSemaphoreGive(state_mutex);
                renderTerminalFullClean();
            } else if (cur_mode == MODE_COMMAND) {
                renderCommandPrompt();
            } else {
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                snapshotState();
                xSemaphoreGive(state_mutex);
                prev_layout = computeLayoutFrom(snap_buf, snap_len, snap_cursor);
                refreshFullClean(prev_layout);
            }
            display_idle = true;
            render_requested = false;
            term_render_requested = false;
            continue;
        }

        // --- Terminal mode ---
        if (cur_mode == MODE_TERMINAL) {
            if (term_render_requested) {
                term_render_requested = false;

                display_idle = false;
                if (connect_status_count > 0) {
                    renderConnectScreen();
                } else {
                    xSemaphoreTake(state_mutex, portMAX_DELAY);
                    snapshotTerminalState();
                    xSemaphoreGive(state_mutex);

                    if (partial_count >= 20) {
                        renderTerminalFullClean();
                    } else {
                        renderTerminal();
                    }
                }
                display_idle = true;
            }
            vTaskDelay(1);
            continue;
        }

        // --- Command mode ---
        if (cur_mode == MODE_COMMAND) {
            if (render_requested) {
                render_requested = false;
                display_idle = false;
                renderCommandPrompt();
                display_idle = true;
            }
            vTaskDelay(1);
            continue;
        }

        // --- Notepad mode ---
        if (!render_requested) {
            vTaskDelay(1);
            continue;
        }
        render_requested = false;

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        snapshotState();

        LayoutInfo cur = computeLayoutFrom(snap_buf, snap_len, snap_cursor);

        if (cur.cursor_line < snap_scroll) {
            snap_scroll = cur.cursor_line;
        }
        if (cur.cursor_line >= snap_scroll + ROWS_PER_SCREEN) {
            snap_scroll = cur.cursor_line - ROWS_PER_SCREEN + 1;
        }
        scroll_line = snap_scroll;
        xSemaphoreGive(state_mutex);

        display_idle = false;
        if (partial_count >= 20) {
            refreshFullClean(cur);
            display_idle = true;
            prev_layout = cur;
            continue;
        }

        int line_delta = abs(cur.cursor_line - prev_layout.cursor_line);
        if (line_delta > ROWS_PER_SCREEN) {
            refreshAllPartial(cur);
            display_idle = true;
            prev_layout = cur;
            continue;
        }

        int old_sl = prev_layout.cursor_line - snap_scroll;
        int new_sl = cur.cursor_line - snap_scroll;

        if (cur.total_lines != prev_layout.total_lines) {
            int from = min(old_sl, new_sl);
            if (from < 0) from = 0;
            refreshLines(from, ROWS_PER_SCREEN - 1, cur);
        } else {
            int min_l = min(old_sl, new_sl);
            int max_l = max(old_sl, new_sl);
            if (min_l < 0) min_l = 0;
            refreshLines(min_l, max_l, cur);
        }
        display_idle = true;

        prev_layout = cur;
    }
}

// --- Keyboard Input (Core 1) ---

void cursorLeft()  { if (cursor_pos > 0) cursor_pos--; }
void cursorRight() { if (cursor_pos < text_len) cursor_pos++; }

void cursorUp() {
    LayoutInfo info = computeLayoutFrom(text_buf, text_len, cursor_pos);
    if (info.cursor_line == 0) return;
    int target_line = info.cursor_line - 1;
    int target_col  = info.cursor_col;
    int line = 0, col = 0;
    for (int i = 0; i <= text_len; i++) {
        if (line == target_line && col == target_col) { cursor_pos = i; return; }
        if (i >= text_len) break;
        if (text_buf[i] == '\n') {
            if (line == target_line) { cursor_pos = i; return; }
            line++; col = 0;
        } else {
            col++;
            if (col >= COLS_PER_LINE) { line++; col = 0; }
        }
    }
}

void cursorDown() {
    LayoutInfo info = computeLayoutFrom(text_buf, text_len, cursor_pos);
    if (info.cursor_line >= info.total_lines - 1) return;
    int target_line = info.cursor_line + 1;
    int target_col  = info.cursor_col;
    int line = 0, col = 0;
    for (int i = 0; i <= text_len; i++) {
        if (line == target_line && col == target_col) { cursor_pos = i; return; }
        if (i >= text_len) break;
        if (text_buf[i] == '\n') {
            if (line == target_line) { cursor_pos = i; return; }
            line++; col = 0;
        } else {
            col++;
            if (col >= COLS_PER_LINE) { line++; col = 0; }
        }
    }
    cursor_pos = text_len;
}

// --- Notepad Key Handler ---

bool handleNotepadKeyPress(int event_code) {
    int key_num = (event_code & 0x7F);
    int idx = key_num - 1;
    int row = idx / KEYPAD_COLS;
    int col_raw = idx % KEYPAD_COLS;
    int col_rev = (KEYPAD_COLS - 1) - col_raw;

    if (row < 0 || row >= KEYPAD_ROWS || col_rev < 0 || col_rev >= KEYPAD_COLS) return false;

    // Modifier keys
    if (IS_LSHIFT(row, col_rev)) { shift_held = !shift_held; return false; }
    if (IS_RSHIFT(row, col_rev)) { nav_mode = !nav_mode; return true; }
    if (IS_SYM(row, col_rev))    { sym_mode = true; return true; }
    if (IS_ALT(row, col_rev))    { alt_mode = !alt_mode; return true; }
    if (IS_MIC(row, col_rev))    {
        if (sym_mode) {
            sym_mode = false;
            if (text_len < MAX_TEXT_LEN) {
                memmove(&text_buf[cursor_pos + 1], &text_buf[cursor_pos], text_len - cursor_pos);
                text_buf[cursor_pos] = '0';
                text_len++; cursor_pos++;
                text_buf[text_len] = '\0';
                file_modified = true;
                return true;
            }
            return false;
        }
        unsigned long now = millis();
        if (now - mic_last_press < MIC_DOUBLE_TAP_MS) {
            mic_last_press = 0;
            app_mode = MODE_TERMINAL;
            if (!ssh_connected && !ssh_connecting) {
                sshConnectAsync();
            }
            return false;
        }
        mic_last_press = now;
        return false;
    }
    if (IS_DEAD(row, col_rev))   { return false; }

    // Nav mode: WASD arrows + backspace delete
    if (nav_mode) {
        char base = keymap_lower[row][col_rev];
        if (base == 'w')      cursorUp();
        else if (base == 'a') cursorLeft();
        else if (base == 's') cursorDown();
        else if (base == 'd') cursorRight();
        else if (base == '\b') {
            if (cursor_pos > 0) {
                memmove(&text_buf[cursor_pos - 1], &text_buf[cursor_pos], text_len - cursor_pos);
                text_len--;
                cursor_pos--;
                text_buf[text_len] = '\0';
                file_modified = true;
            } else return false;
        }
        else if (alt_mode && base == 'f') { alt_mode = false; partial_count = 100; render_requested = true; return false; }
        else return false;
        return true;
    }

    char c;
    if (sym_mode)        { c = keymap_sym[row][col_rev]; sym_mode = false; }
    else if (shift_held) c = keymap_upper[row][col_rev];
    else                 c = keymap_lower[row][col_rev];

    if (c == 0) return false;

    if (c == '\b') {
        if (cursor_pos > 0) {
            memmove(&text_buf[cursor_pos - 1], &text_buf[cursor_pos], text_len - cursor_pos);
            text_len--;
            cursor_pos--;
            text_buf[text_len] = '\0';
            file_modified = true;
            return true;
        }
        return false;
    }

    if (c == '\n' || (c >= ' ' && c <= '~')) {
        if (text_len < MAX_TEXT_LEN) {
            memmove(&text_buf[cursor_pos + 1], &text_buf[cursor_pos], text_len - cursor_pos);
            text_buf[cursor_pos] = c;
            text_len++;
            cursor_pos++;
            text_buf[text_len] = '\0';
            file_modified = true;
            if (shift_held) shift_held = false;
            return true;
        }
    }

    return false;
}

// --- Terminal Key Handler ---

bool handleTerminalKeyPress(int event_code) {
    int key_num = (event_code & 0x7F);
    int idx = key_num - 1;
    int row = idx / KEYPAD_COLS;
    int col_raw = idx % KEYPAD_COLS;
    int col_rev = (KEYPAD_COLS - 1) - col_raw;

    if (row < 0 || row >= KEYPAD_ROWS || col_rev < 0 || col_rev >= KEYPAD_COLS) return false;

    if (IS_LSHIFT(row, col_rev)) { shift_held = !shift_held; return false; }
    if (IS_RSHIFT(row, col_rev)) { nav_mode = !nav_mode; return true; }
    if (IS_SYM(row, col_rev))    { sym_mode = true; return true; }
    if (IS_ALT(row, col_rev))    { alt_mode = !alt_mode; return true; }
    if (IS_MIC(row, col_rev))    {
        if (sym_mode) { sym_mode = false; sshSendKey('0'); return false; }
        unsigned long now = millis();
        if (now - mic_last_press < MIC_DOUBLE_TAP_MS) {
            mic_last_press = 0;
            app_mode = MODE_NOTEPAD;
            return false;
        }
        mic_last_press = now;
        return false;
    }
    if (IS_DEAD(row, col_rev))   { return false; }

    // Nav mode: WASD sends arrow escape sequences
    if (nav_mode) {
        char base = keymap_lower[row][col_rev];
        if (base == 'w') { sshSendString("\x1b[A", 3); return false; }
        if (base == 's') { sshSendString("\x1b[B", 3); return false; }
        if (base == 'd') { sshSendString("\x1b[C", 3); return false; }
        if (base == 'a') { sshSendString("\x1b[D", 3); return false; }
        if (base == '\b') { sshSendKey(0x7F); return false; }
        return false;
    }

    // Alt = Ctrl modifier
    if (alt_mode) {
        char base = keymap_lower[row][col_rev];
        if (base == ' ') { sshSendKey(0x1B); alt_mode = false; return true; }
        if (base == '\n') { sshSendKey('\t'); alt_mode = false; return true; }
        if (base == '\b') { sshSendKey(0x7F); alt_mode = false; return false; }
        if (base >= 'a' && base <= 'z') {
            sshSendKey(base - 'a' + 1);
            alt_mode = false;
            return false;
        }
        return false;
    }

    char c;
    if (sym_mode)        { c = keymap_sym[row][col_rev]; sym_mode = false; }
    else if (shift_held) c = keymap_upper[row][col_rev];
    else                 c = keymap_lower[row][col_rev];

    if (c == 0) return false;

    if (c == '\b') { sshSendKey(0x7F); return false; }
    if (c == '\n') { sshSendKey('\r'); return false; }

    if (c >= ' ' && c <= '~') {
        sshSendKey(c);
        if (shift_held) shift_held = false;
        return false;
    }

    return false;
}

// --- Command Processor ---

// --- SCP Upload ---

static volatile bool upload_running = false;
static volatile int upload_done_count = 0;
static volatile int upload_total_count = 0;

bool scpPushFile(ssh_scp scp, const char* sd_path, const char* remote_name) {
    sdAcquire();
    File f = SD.open(sd_path, FILE_READ);
    if (!f) { sdRelease(); return false; }
    size_t sz = f.size();

    // Read entire file into memory (files are small, max 4K)
    char buf[MAX_TEXT_LEN];
    int n = f.read((uint8_t*)buf, sz);
    f.close();
    sdRelease();

    if (n <= 0) return false;
    if (ssh_scp_push_file(scp, remote_name, n, 0644) != SSH_OK) return false;
    return ssh_scp_write(scp, buf, n) == SSH_OK;
}

void uploadTask(void* param) {
    upload_running = true;
    upload_done_count = 0;

    // List flat files at root
    int n = listDirectory("/");
    upload_total_count = 0;
    for (int i = 0; i < n; i++) {
        if (!file_list[i].is_dir) upload_total_count++;
    }
    cmdSetResult("Uploading %d files...", upload_total_count);
    render_requested = true;

    // Create SCP session using existing SSH connection's session
    ssh_scp scp = ssh_scp_new(ssh_sess, SSH_SCP_WRITE, "tdeck");
    if (!scp) {
        cmdSetResult("SCP: init failed");
        render_requested = true;
        upload_running = false;
        vTaskDelete(NULL);
        return;
    }
    if (ssh_scp_init(scp) != SSH_OK) {
        cmdSetResult("SCP: %s", ssh_get_error(ssh_sess));
        ssh_scp_free(scp);
        render_requested = true;
        upload_running = false;
        vTaskDelete(NULL);
        return;
    }

    // Push each file
    for (int i = 0; i < n; i++) {
        if (file_list[i].is_dir) continue;
        String path = "/" + String(file_list[i].name);
        if (scpPushFile(scp, path.c_str(), file_list[i].name)) {
            upload_done_count++;
            render_requested = true;
        }
    }

    ssh_scp_close(scp);
    ssh_scp_free(scp);

    cmdSetResult("Upload done: %d files", upload_done_count);
    render_requested = true;
    upload_running = false;
    vTaskDelete(NULL);
}

// --- SCP Download ---

static volatile bool download_running = false;
static volatile int download_done_count = 0;
static volatile int download_total_count = 0;

bool scpPullFile(ssh_scp scp, const char* remote_name, size_t size) {
    char buf[MAX_TEXT_LEN];
    size_t remaining = size;
    size_t offset = 0;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        int n = ssh_scp_read(scp, buf + offset, chunk);
        if (n < 0) return false;
        offset += n;
        remaining -= n;
    }

    String path = "/" + String(remote_name);
    sdAcquire();
    File f = SD.open(path.c_str(), FILE_WRITE);
    if (!f) { sdRelease(); return false; }
    f.write((uint8_t*)buf, offset);
    f.close();
    sdRelease();
    return true;
}

void downloadTask(void* param) {
    download_running = true;
    download_done_count = 0;
    download_total_count = 0;

    ssh_scp scp = ssh_scp_new(ssh_sess, SSH_SCP_READ | SSH_SCP_RECURSIVE, "tdeck");
    if (!scp) {
        cmdSetResult("SCP: init failed");
        render_requested = true;
        download_running = false;
        vTaskDelete(NULL);
        return;
    }
    if (ssh_scp_init(scp) != SSH_OK) {
        cmdSetResult("SCP: %s", ssh_get_error(ssh_sess));
        ssh_scp_free(scp);
        render_requested = true;
        download_running = false;
        vTaskDelete(NULL);
        return;
    }

    cmdSetResult("Downloading...");
    render_requested = true;

    int r;
    while ((r = ssh_scp_pull_request(scp)) != SSH_SCP_REQUEST_EOF) {
        if (r == SSH_SCP_REQUEST_NEWDIR) {
            ssh_scp_accept_request(scp);
            continue;
        }
        if (r == SSH_SCP_REQUEST_ENDDIR) {
            continue;
        }
        if (r == SSH_SCP_REQUEST_NEWFILE) {
            size_t size = ssh_scp_request_get_size(scp);
            const char* name = ssh_scp_request_get_filename(scp);
            download_total_count++;
            ssh_scp_accept_request(scp);
            if (scpPullFile(scp, name, size)) {
                download_done_count++;
                render_requested = true;
            }
            continue;
        }
        if (r == SSH_ERROR) break;
    }

    ssh_scp_close(scp);
    ssh_scp_free(scp);

    cmdSetResult("Download done: %d files", download_done_count);
    render_requested = true;
    download_running = false;
    vTaskDelete(NULL);
}

// --- Power Off Art ---

static const char poweroff_art[] PROGMEM =
    "                    .\n"
    "                    .ll,.\n"
    "                     ,,'.\n"
    "\n"
    "                         ..\n"
    "                          ..            ...',;;;;,..\n"
    "                           ..       ..',;:::;;:;;:,'.\n"
    "                            ..   .',;:;;;;::;::::,'',.\n"
    "                             .,''::;;;;;;;;;;:::,'',',','.\n"
    "                          .';;;,,',;;;;;::::::;,',,,,',,.\n"
    "                        .,cc:;;,''';:::ccclc:;;,,'',,'',,.\n"
    "                      .;cccccc:;;:ccllllolc::;;;;,'',''',,'..\n"
    "                    .;cc:cccllclllllllollc:::::;;;,'',',,,'''.\n"
    "                   ,lccclllllllllloooolc::::::::;;;,,,',,'.'''........\n"
    "                 .:lccllllllllllooodoooc:::::::::;;;;,,,'.''',;::;,,,,,'.\n"
    "                .locclllllooooodxxxddddoc:::::::::;;;;,'''',:::;,,''''''.\n"
    "                .dxllloodxkO000OOOOxddddol::::::::::;,,,'''cxkko;''',,''''.\n"
    "                 'x0OO00KXNWWWWX0OOOkddddol:::::::;;;;;:;,,lkkko,''',;,'''.\n"
    "                  ,ONNXXNWWWWWWWN0OOOkdoodol:::::::::;;;:,'cxk0x:'''''''',.\n"
    "                   .kNNXXNWWWWWWWN0OOOkdooddl::::c::::;;,,';dkOkc,,''','',.\n"
    "                    .dNNXXNWWWWWWWN0OOOkxoollccc:::::::;,,'.;lllc;',,,,,,.\n"
    "                     .lXNXXNWWNWWWWNKOkxdlclodolc:::::::;;,,'';:::;,,''..\n"
    "                     .,lKNXXNWWWNXK0kdlodxxxxxxolc::::::::;;'..'.....\n"
    "                    .lo,cxkxxkkxxdddxOO0KK0kxxxxolcc::::::,....'.\n"
    "                    .okclxkkkOO0KKXNNWNXOddxxxxxxdlcc:;;,'.',,'..         .. .....\n"
    "                     :0xlxKWWNNXXNWWWWNx,,,,lxxxxxdoc;'..',,,,,....     ..        ..\n"
    "                     .l0kod0XkccclONWWNx,::,lOxxxdl:,'';;:::;'......  .            ..    ..\n"
    "                       ;xkdoxdccc,cXWNWXOddk0Odc;,',:clllcc;.. .......          ....'''',,''.\n"
    "                        .';,.:dxdd0WWWNX0Oxol:::clddxdool:'.....  ..... .....',,,,,,,,,;,''''.\n"
    "                               .cllllxkdoodxO000Okkkkxo:'........   .....''',,,,,,,,,;;,''''''.\n"
    "                                     .xXXKXNWWXXX0kdc,'....'......  . .......,,,,,,,:;''.....''..''...\n"
    "                                      .l0KKKXKOxo:'. .''''',;,'..............,,;;;;c;'.....'::,,,,,,,,.\n"
    "                                        .'''...     .::;;:c:;;,'...........';:::::c;'.....;ol,,,''''',,'.\n"
    "                                                  .cxxoc;:c:;::,.........';ccllc:c;'..',':kOl,,''','''',.\n"
    "                                                 ;xkddccc:cc:;;;,,,'..',;codddocc;''.';;,:ddc,,'',;,..'''.\n"
    "                                               .:kkxxx::c:lcccc:;,;::::lodddddll:,,'..''.,xOo;,,'','..','\n"
    "                                              .oOOkkkx:,llc:;:clooooooddddxxollc,,,,......o0x:,,,''''',,.\n"
    "                                             .d00OOOxOkdodddddoddddxkkkkkkkxocc;,,,,'......;:;,'',,,,,'.\n"
    "                                            .dkOK000OOOOOkkOkxxxkOO00KKK000dcc;,,,,,''........',,'''....\n"
    "                                       .;:;:xKKKK00000000000KKKXXXXNNXXKKK0occ;,,,,,,''...... .:l:;'''''.\n"
    "                                     .cxOOokKXNXKKKXXXXNNNNNXXKK00000OOOkO0x:::,,,,,,,'''......cOxoc,'''.\n"
    "                                     :kOXkdXXWWNNXXKK000000000000000KXKK0Okkl::;,,,,,,,','''''.,kOdl;'''.\n"
    "                                     o0ONxxXK0KKK000000KKXXXXNNNXXX0KXXKKOkOxc::;,,,,,,,,,''''''o0xo:,''.\n"
    "                                     l0OXOd0NNNNNNNWNNNXXK0Okxxdoolc:ckXX0Okko:::,,,,,,,,,,,,,,';kOdo;'''.\n"
    "                                     'kOOXxxNWNK0OOkxdooc:;,,,,,,,;,,':OXKOkkxc;:;,',',,,,,,,,,,'l0kdc,''.\n"
    "                          ,,          'dxxkdOW0ollllol;;;;;;;;;,,,,,,,'lKXOkxkd:::;,',,,,,,,,,,,';O0xo;''..\n"
    "                         'ol         .cdo:lloK0ddc:::ol;,,;,,',;,;;;;,,,dXKOxxxl;::;,,,,,,,,,,,,''oKOd:.....\n"
    "                          ,'       .:OKOd:...cdccdko;;:;,,;lolll;;;,;;,,:kKOkxkxc:::,,,,,,,,,,,,,',loc;,'....\n"
    "                          ,'      ,xKOxd:.   .oxoodoc;,,,',cdc;::,;,,;,,'cOOkkxkd:::;,,,,,,,,,,,,,''oOdl:'..\n"
    "                          ,     .oK0xo:.   .'dKOl;;lxxc;,,,';c:ldldo:;,,,,oOkkkxxl;::;',,,,,,,,,,,,'cOkoc,...\n"
    "                    ,:cllclc:::;lxdl:..  .:do::cooc:ddc;,,,,,;;;::::;;,',';xOkkxxdc;::,,,,,,,,,,,,,'cOxl:,''.\n"
    "                  ckKNWWNNNXXXXK0ko:,.  .cxo,..ckkkd:;cl,',,,'',,,;::;,',,'ckOxdddo:::;,,,,,,,,,,,,,'cOxl:,''.\n"
    "                 c0KKKKKKKKKKKK0Odlc:'   ......;xkl:lcoxc,,',,',;''ldlllllclddollllc::;,''',,,,,,,',.,xkdl;''.\n"
    "                 dNWNXNNNNNNXXNN0olc;c;..:::cc,..;okOocol;,','.....,::ccccllllllc:cllccc:::;;:;;;,,'..coc;'.....\n"
    "               xxkNKl:xXNNKl:oOK0oc:lxl,,,.....'oxlc:,..,,,,''.....',,,,,,;;;;;::,;::::::cccclccc::;,;,',,,,,,,'''.\n"
    "                :xNKocxXNNKo:dOKOlc;;c,.     .cdo:.'ll,.:x:''.....',,,,,''''',,,''',,,,,,,,;;;;;;;;;,,,..cl;,,,,,','.\n"
    "                 oNWNKKKK0000XXXklc:,;'   .;ll:,.  .d0d::l;,,,:lolc:;;,,,'',,;,,'',,,,,,,,,,,',,',,,'''.:l,,,,,,'''',.\n"
    "                 lNWXOkxxxxdkKXXklccc:,  .:l:'.     .xNOxdodk0KOxxol:,,;clcc::;,,,,,,,;;;;,,,,,,,,,'.'.'lc,,,,:,.',','\n"
    "                 'coddddxxddddooc;,;;     ..         .kWXXXXXXKoldcol,'':oxxxddollc:;,,,''''',,;,,''''.'lc,,'''..''',.\n"
    "                       ,:;,''.                        'ONXXXXXKklol:cl;';ldddddddddolc;;;,''......  .....,;,,,,,','.\n"
    "                       cxoc:;'                         ,KNXXXXK0kolcclcclodddddddddolc;;;,''......  .................\n"
    "               .'cdkOklokocc:,,,'',;::;,.               ,k0KXXKKK00Okkxxddddddoooolc:;,''... .....        ........\n"
    "        ..;clc;lOXXXXKxddoccclllddxdll:;;,'...            ..'''''''''..''''...........    ........\n"
    "        ;ONWNkx0KKKKKKKKK000000KKK0d:;,:kXK0kl.                        .'.........................\n"
    "        'oKN0dkWWK000KKKKK0K000XXK0o;;'cOK0Ol'.                        .',;:::;,''...... .........\n"
    "        .'lkxlkWXdcc::::ccllclokKK0o;,':kkkl'..                 .....  .,;;:cc:;'.....................\n"
    "      ..lxc;;,dNXdc:codoccc::ccxKK0o;,.':c:',:;'             .cxO0Oo::c;,;::lll:,................''''.\n"
    "     .;k0d:. .oNKdc:lkkxolo:;ccxKK0o:;,,'.  'odc,.          ,kKXX0olxxl;'.,::::;,................'''''.\n"
    "    .cKKx:.   oNKd:;cdlc::c;:ccxKK0o::;:'    .dOo;.        ,OKXNXol0X0kxdlloooool:;;,,',;:cc:,'......',,,,,,......\n"
    "   .,dxo;.   .oXKd:;:dkxxxdolc:d00Oo::;:'     .ox:'.      .dKKXXO:l00000OOOOOOOOOOOkxxxxxkxl;'.....'cdddol:,,,,,,,'.\n"
    "   ,lc,.     .oXKdc::lxkkdlcc:;oOOOo::;:'       ',,,.     'O0O00x;l0000Okkkkkkkkkxdddddddo:,,..,;':xOOkkx:,,,,,,,,,,.\n"
    "  .d0d;.     .lKKkolloxkkdlllllxOOko::;:.       .:dl;     ,0K0O0d,;xOkkkkkkkkkxxxxdddddddo;,...,.:OOOkxx:',,'..'',,,'\n"
    "  ,KKx,       cO000OOOOOkkkkkkkkkOko:;,,.        cKxc.    ;0KO00d,.;dxddxxxxdddddolooooooc;'.....dOxxxkl',,'.''..',,'.\n"
    "  cX0o.       ,xkkkkkkkkkxxxxxxxxxxl;,,,.        'OOo;   .xNXKKx:,'.,ldooooooollllcllclllc,'....'xxoodd:',,.';,..',,.\n"
    "  ,ol;.       .xOkOOOOkkkkkkkkkkkxxl::;.         .co:,. .dXXXXk:,,''..,:ccccccccccc:::::::,'... ,kxdddl:',,'.''..','.\n"
    ".:dxdc.        ':ccc::::ccccccccccc;'..          .;:,,'.lXXXX0c,;,'','.......'',,,,;;;;,;;;,,'..dOxdxd:'',,,'.''',,.\n"
    "lX0xkOd,         ;:.'.         ...''.           :kxodxl;cOXNKl,;;;,,'.               ..........dOxddd:,'',,,,,,'.\n"
    "'OKc..ckc.       'kd:;.          :o:;.          'Ox..'lo,'lOOo;;;;;;,.                        .oOxdddc','',,,,,,'.\n"
    ".cx:. 'c.        l0dl,           l0dl,          .c;  .:c',ll:,,;,,;,.                        .lOxdddl:',,,'''...\n"
    " .             .kOoc.           :Oxl;.              .;clkkoc;,,',,.                         ckdoddl;,,,,,,'.\n"
    "             ..:xoc,            ;xo:,.             .dKKXKxo:,''''.                         .clc:cc:',,,''.\n"
    "            'xKK0Oo;..         ,dkxdl;..          .xXXX0xo:,,;,..                          ,oddoc,''',','.\n"
    ".            .o0KXX0o;'.       .:dKXX0x;'.       ...ckO0koo;',;,.                           .okkkdc,''''''.\n"
    ";.           'xKXXXOl:;'       'cxKKK0xc,'.    .,oo:;clc;,,',;,..                          .okkkdc,','..'....        .........\n"
    "::dxkkOOOOkOdoOKXXKkc:;:dOOOOOkocd0000kl,';dOOkodo;',;;;;cc:;;,;cllllllccc:::clllllllcccc::dkkxdc:',,,'..',;;;;,,,;;;;;;;;''''\n"
    "l:coodkXWWWWOd0KKKKxc:;:0WWWWWXdcdO0OOko;,,oXWKdlokkkkxdooc:;;.';::;coodooooddxolllc:cc:;;okxxxc:',,,''.';;;;;,,,,,;:;;;:;''..\n"
    "l::dxk0NWWWNkx0KKKKd::;:OWWWWWXxcdOOOOkdc,,:ONKd;,:cloooc;;;:;,;:;'';:cc::::;;;,,,,,,,,,,,;::c:,'''..'..''''..','''''''''.',;,\n"
    "ddx0K00OkKNKxkKKKK0d::;c0WWWWWXxcdOOOOkxl;;,oXNKdlc:,''''.',;;;,'........'.''''','''.';lc'',;;'''..,;;,,,,,,,...........,:cc::\n"
    "NKkddxOOO00xdOKKKKOl;:;:xWWWWNKdcokOOkkxo:,.;ONWWNNKkollllc:,'',,;::::::ccccccc::cllc:lo:,,,,,'';;:;,;:cc:;;,'.........,cc,;;,\n"
    "OOxooddokNNxd00OkOkl;,,:OWWWWXkolokOkxxdoc,';ONNNNXXXX0kkkkkkkkOOOxddkOOkkkkxxxoodddo::::llccc::;,;;;;;;,,,,,,',,'.....,:,,:;,\n"
    "O0OxddxxONNdcodddddc,'.,ldxkOkdooldkdoddo:,.':cccclllloooololloool:;:cllc::cc::::;,,,'...''''',,'.',,,''',,,,,:;,,,'....,;,,,,\n"
    "WNWWWWWWWWWk;,'''.........',,;;;;,,,,,,,'............'''''',,,;::,''''.'''',,'''''''''...'''.':::;'.......'',:c,,:cc::;;,,,,,,\n";

#define POWEROFF_ART_LINES 92
#define POWEROFF_ART_PX_W  2
#define POWEROFF_ART_PX_H  3

void powerOff() {
    // Render ASCII art as pixel bitmap on e-ink, then deep sleep
    const int art_height = POWEROFF_ART_LINES * POWEROFF_ART_PX_H;
    const int y_offset = (SCREEN_H - art_height) / 2;

    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        const char* p = poweroff_art;
        int row = 0;
        int col = 0;
        while (*p) {
            if (*p == '\n') {
                row++;
                col = 0;
            } else {
                if (*p != ' ') {
                    int px = col * POWEROFF_ART_PX_W;
                    int py = y_offset + row * POWEROFF_ART_PX_H;
                    if (px + POWEROFF_ART_PX_W <= SCREEN_W && py + POWEROFF_ART_PX_H <= SCREEN_H && py >= 0) {
                        display.fillRect(px, py, POWEROFF_ART_PX_W, POWEROFF_ART_PX_H, GxEPD_BLACK);
                    }
                }
                col++;
            }
            p++;
        }
    } while (display.nextPage());

    delay(100);
    display.hibernate();

    // Turn off modem if on
    if (modem_state != MODEM_OFF) {
        modemPowerOff();
    }

    // Disconnect WiFi
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    // Enter deep sleep with no wakeup source (only reset wakes)
    esp_deep_sleep_start();
}

// --- Command Processor ---

void executeCommand(const char* cmd) {
    // Parse command word and argument
    char word[CMD_BUF_LEN + 1];
    char arg[CMD_BUF_LEN + 1];
    word[0] = '\0';
    arg[0] = '\0';

    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') { cmd_result_valid = false; return; }

    int wi = 0;
    while (*cmd && *cmd != ' ' && wi < CMD_BUF_LEN) word[wi++] = *cmd++;
    word[wi] = '\0';
    while (*cmd == ' ') cmd++;
    strncpy(arg, cmd, CMD_BUF_LEN);
    arg[CMD_BUF_LEN] = '\0';

    // --- File commands (flat, all files at /) ---
    if (strcmp(word, "l") == 0 || strcmp(word, "ls") == 0) {
        int n = listDirectory("/");
        if (n < 0) {
            cmdSetResult("Can't read SD");
        } else if (n == 0) {
            cmdSetResult("(empty)");
        } else {
            cmdClearResult();
            for (int i = 0; i < n && i < CMD_RESULT_LINES; i++) {
                if (file_list[i].is_dir) {
                    cmdAddLine("[%s]", file_list[i].name);
                } else {
                    cmdAddLine("%s %dB", file_list[i].name, (int)file_list[i].size);
                }
            }
            if (n > CMD_RESULT_LINES) cmdAddLine("... +%d more", n - CMD_RESULT_LINES);
        }
    } else if (strcmp(word, "e") == 0 || strcmp(word, "edit") == 0) {
        if (arg[0] == '\0') { cmdSetResult("e <file>"); }
        else {
            autoSaveDirty();
            String path = "/" + String(arg);
            if (loadFromFile(path.c_str())) {
                current_file = path;
                cmdSetResult("Loaded %s (%d B)", arg, text_len);
                app_mode = MODE_NOTEPAD;
            } else {
                text_len = 0; cursor_pos = 0; scroll_line = 0;
                text_buf[0] = '\0';
                current_file = path;
                file_modified = false;
                cmdSetResult("New: %s", arg);
                app_mode = MODE_NOTEPAD;
            }
        }
    } else if (strcmp(word, "w") == 0 || strcmp(word, "save") == 0) {
        if (arg[0] != '\0') {
            current_file = "/" + String(arg);
        } else if (current_file.length() == 0) {
            current_file = "/UNSAVED";
        }
        if (saveToFile(current_file.c_str())) {
            cmdSetResult("Saved %s (%d B)", current_file.c_str(), text_len);
        } else {
            cmdSetResult("Save failed");
        }
    } else if (strcmp(word, "n") == 0 || strcmp(word, "new") == 0) {
        autoSaveDirty();
        text_len = 0; cursor_pos = 0; scroll_line = 0;
        text_buf[0] = '\0';
        current_file = "";
        file_modified = false;
        cmdSetResult("New buffer");
        app_mode = MODE_NOTEPAD;
    } else if (strcmp(word, "r") == 0 || strcmp(word, "rm") == 0) {
        if (arg[0] == '\0') { cmdSetResult("r <name>"); }
        else {
            String path = "/" + String(arg);
            sdAcquire();
            bool ok = SD.remove(path.c_str());
            sdRelease();
            cmdSetResult(ok ? "Removed %s" : "Failed: %s", arg);
        }
    } else if (strcmp(word, "u") == 0 || strcmp(word, "upload") == 0) {
        if (!ssh_connected) {
            cmdSetResult("SSH not connected");
        } else if (upload_running) {
            cmdSetResult("Upload in progress...");
        } else {
            char mkdir_cmd[64];
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p ~/tdeck\n");
            ssh_channel_write(ssh_chan, mkdir_cmd, strlen(mkdir_cmd));
            vTaskDelay(pdMS_TO_TICKS(500));
            char drain[256];
            ssh_channel_read_nonblocking(ssh_chan, drain, sizeof(drain), 0);
            xTaskCreatePinnedToCore(uploadTask, "upload", 16384, NULL, 1, NULL, 1);
            cmdSetResult("Starting upload...");
        }
    } else if (strcmp(word, "d") == 0 || strcmp(word, "download") == 0) {
        if (!ssh_connected) {
            cmdSetResult("SSH not connected");
        } else if (download_running) {
            cmdSetResult("Download in progress...");
        } else {
            xTaskCreatePinnedToCore(downloadTask, "download", 16384, NULL, 1, NULL, 1);
            cmdSetResult("Starting download...");
        }
    }
    // --- Other commands ---
    else if (strcmp(word, "p") == 0 || strcmp(word, "paste") == 0) {
        if (!ssh_connected || !ssh_chan) {
            cmdSetResult("SSH not connected");
        } else if (text_len == 0) {
            cmdSetResult("Notepad empty");
        } else {
            for (int i = 0; i < text_len; i += 64) {
                int chunk = (text_len - i > 64) ? 64 : (text_len - i);
                ssh_channel_write(ssh_chan, &text_buf[i], chunk);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            cmdSetResult("Pasted %d chars", text_len);
        }
    } else if (strcmp(word, "dc") == 0) {
        sshDisconnect();
        cmdSetResult("Disconnected");
    } else if (strcmp(word, "f") == 0 || strcmp(word, "refresh") == 0) {
        partial_count = 100;
        cmdSetResult("Full refresh queued");
    } else if (strcmp(word, "4") == 0 || strcmp(word, "4g") == 0) {
        if (modem_state == MODEM_OFF || modem_state == MODEM_FAILED) {
            modemStartAsync();
            cmdSetResult("4G starting...");
        } else if (modem_state == MODEM_PPP_UP) {
            modemStop();
            cmdSetResult("4G stopped");
        } else {
            cmdSetResult("4G busy...");
        }
    } else if (strcmp(word, "s") == 0 || strcmp(word, "status") == 0) {
        const char* ws = "off";
        if (wifi_state == WIFI_CONNECTED) ws = "ok";
        else if (wifi_state == WIFI_CONNECTING) ws = "...";
        const char* ms = "off";
        if (modem_state == MODEM_PPP_UP) ms = "ok";
        else if (modem_state >= MODEM_POWERING_ON && modem_state <= MODEM_REGISTERED) ms = "...";
        else if (modem_state == MODEM_FAILED) ms = "fail";
        cmdClearResult();
        cmdAddLine("WiFi:%s 4G:%s SSH:%s", ws, ms, ssh_connected ? "ok" : "off");
        cmdAddLine("Bat:%d%% Heap:%dK", battery_pct, ESP.getFreeHeap() / 1024);
        if (current_file.length() > 0) cmdAddLine("File:%s%s", current_file.c_str(), file_modified ? "*" : "");
    } else if (strcmp(word, "off") == 0) {
        poweroff_requested = true;
    } else if (strcmp(word, "?") == 0 || strcmp(word, "h") == 0 || strcmp(word, "help") == 0) {
        cmdClearResult();
        cmdAddLine("(l)ist (e)dit (w)rite (n)ew");
        cmdAddLine("(r)m (u)pload (d)ownload");
        cmdAddLine("(p)aste dc re(f)resh (4)g off (h)elp");
    } else {
        cmdSetResult("Unknown: %s (?=help)", word);
    }
}

bool handleCommandKeyPress(int event_code) {
    int key_num = (event_code & 0x7F);
    int idx = key_num - 1;
    int row = idx / KEYPAD_COLS;
    int col_raw = idx % KEYPAD_COLS;
    int col_rev = (KEYPAD_COLS - 1) - col_raw;

    if (row < 0 || row >= KEYPAD_ROWS || col_rev < 0 || col_rev >= KEYPAD_COLS) return false;

    if (IS_SHIFT(row, col_rev)) { shift_held = !shift_held; return false; }
    if (IS_SYM(row, col_rev))   { sym_mode = true; return true; }
    if (IS_ALT(row, col_rev))   { return false; }
    if (IS_MIC(row, col_rev))   {
        if (sym_mode) {
            sym_mode = false;
            if (cmd_len < CMD_BUF_LEN) {
                cmd_buf[cmd_len++] = '0';
                cmd_buf[cmd_len] = '\0';
                return true;
            }
            return false;
        }
        app_mode = cmd_return_mode;
        return false;
    }
    if (IS_DEAD(row, col_rev))  { return false; }

    char c;
    if (sym_mode)        { c = keymap_sym[row][col_rev]; sym_mode = false; }
    else if (shift_held) c = keymap_upper[row][col_rev];
    else                 c = keymap_lower[row][col_rev];

    if (c == 0) return false;

    if (c == '\b') {
        if (cmd_len > 0) {
            cmd_len--;
            cmd_buf[cmd_len] = '\0';
            return true;
        }
        return false;
    }

    if (c == '\n') {
        executeCommand(cmd_buf);
        cmd_len = 0;
        cmd_buf[0] = '\0';
        return true;
    }

    if (c >= ' ' && c <= '~' && cmd_len < CMD_BUF_LEN) {
        cmd_buf[cmd_len++] = c;
        cmd_buf[cmd_len] = '\0';
        if (shift_held) shift_held = false;
        return true;
    }

    return false;
}

void renderCommandPrompt() {
    partial_count++;
    // Half screen: top half stays (notepad/terminal content), bottom half is command area
    int cmd_area_y = SCREEN_H / 2;
    int region_h = SCREEN_H - cmd_area_y;

    display.setPartialWindow(0, cmd_area_y, SCREEN_W, region_h);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(NULL);

        // Separator line
        display.drawLine(0, cmd_area_y, SCREEN_W, cmd_area_y, GxEPD_BLACK);

        int y = cmd_area_y + 2;

        // Show multi-line result
        if (cmd_result_valid) {
            for (int i = 0; i < cmd_result_count; i++) {
                display.setCursor(MARGIN_X, y);
                display.print(cmd_result[i]);
                y += CHAR_H;
                if (y >= SCREEN_H - STATUS_H - CHAR_H - 2) break;
            }
        }

        // Draw prompt line at bottom of command area (above status bar)
        int py = SCREEN_H - STATUS_H - CHAR_H - 2;
        display.setCursor(MARGIN_X, py);
        display.print("> ");
        display.print(cmd_buf);
        // Cursor
        int cx = MARGIN_X + (cmd_len + 2) * CHAR_W;
        display.fillRect(cx, py - 1, CHAR_W, CHAR_H, GxEPD_BLACK);

        // Status bar
        int bar_y = SCREEN_H - STATUS_H;
        display.fillRect(0, bar_y, SCREEN_W, STATUS_H, GxEPD_BLACK);
        display.setTextColor(GxEPD_WHITE);
        display.setCursor(2, bar_y + 1);
        if (upload_running) {
            char ul[40];
            snprintf(ul, sizeof(ul), "Upload: %d/%d", (int)upload_done_count, (int)upload_total_count);
            display.print(ul);
        } else if (download_running) {
            char dl[40];
            snprintf(dl, sizeof(dl), "Download: %d/%d", (int)download_done_count, (int)download_total_count);
            display.print(dl);
        } else {
            display.print("[CMD] ? help | MIC exit");
        }
    } while (display.nextPage());
}

// --- Setup & Loop ---

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("T-Deck Pro Notepad + Terminal starting...");

    // Disable task watchdog — SSH blocking calls would trigger it
    esp_task_wdt_deinit();

    // Disable unused peripherals
    pinMode(BOARD_LORA_EN, OUTPUT);        digitalWrite(BOARD_LORA_EN, LOW);
    pinMode(BOARD_GPS_EN, OUTPUT);         digitalWrite(BOARD_GPS_EN, LOW);
    pinMode(BOARD_1V8_EN, OUTPUT);         digitalWrite(BOARD_1V8_EN, LOW);
    // Modem off by default (Alt+M to enable)
    pinMode(BOARD_MODEM_POWER_EN, OUTPUT); digitalWrite(BOARD_MODEM_POWER_EN, LOW);
    pinMode(BOARD_MODEM_PWRKEY, OUTPUT);   digitalWrite(BOARD_MODEM_PWRKEY, LOW);

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

    Serial.println("Ready. Double-tap MIC to switch modes. Single-tap MIC for commands.");
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
}

// Core 1: keyboard polling — never blocks on display
void loop() {
    // Check WiFi / modem status (lightweight cached state check)
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

    // MIC single-tap timeout → open command processor
    if (mic_last_press > 0 && (millis() - mic_last_press >= MIC_DOUBLE_TAP_MS)) {
        mic_last_press = 0;
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        cmd_return_mode = app_mode;
        cmd_len = 0;
        cmd_buf[0] = '\0';
        cmd_result_valid = false;
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
            } else if (cur == MODE_COMMAND) {
                render_requested = true;
            }
        }
    }
}
