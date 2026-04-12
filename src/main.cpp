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

#ifndef TDECK_AGENT_DEBUG
#define TDECK_AGENT_DEBUG 0
#endif

#if TDECK_AGENT_DEBUG
#define SERIAL_LOG_BEGIN(baud) Serial.begin(baud)
#define SERIAL_LOGF(...) Serial.printf(__VA_ARGS__)
#define SERIAL_LOG(...) Serial.print(__VA_ARGS__)
#define SERIAL_LOGLN(...) Serial.println(__VA_ARGS__)
#else
#define SERIAL_LOG_BEGIN(baud) do { (void)(baud); } while (0)
#define SERIAL_LOGF(...) do {} while (0)
#define SERIAL_LOG(...) do {} while (0)
#define SERIAL_LOGLN(...) do {} while (0)
#endif

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

// --- VPN Config (loaded from SD /CONFIG) ---
static char   config_vpn_privkey[64] = "";
static char   config_vpn_pubkey[64]  = "";
static char   config_vpn_psk[64]     = "";
static char   config_vpn_ip[32]      = "";
static char   config_vpn_endpoint[64] = "";
static int    config_vpn_port        = 51820;
static char   config_vpn_dns[32]     = "";
static char   config_time_tz[64]     = "UTC0";
static char   config_msh_channel[32] = "";
static char   config_msh_psk[96]     = "";
static WireGuard wg;
static bool   vpn_connected = false;
static bool vpnConfigured() { return config_vpn_privkey[0] != '\0'; }
static bool vpnActive() { return vpn_connected && wg.is_initialized(); }

// Forward declarations
void connectMsg(const char* fmt, ...);
void powerOff();
static int partial_count = 0;
static volatile uint32_t perf_window_start_ms = 0;
static volatile uint32_t perf_heap_min5_kb = 0;
static volatile uint32_t perf_render_max5_ms = 0;
static volatile uint32_t perf_loop_max5_ms = 0;
static constexpr uint32_t PERF_WINDOW_MS = 5000U;
static void perfRecordRenderMs(uint32_t render_ms);
static void perfLoopTick();
static void buildPerfStatusCompact(char* out, size_t out_len);
static void perfMaybeRollWindow(uint32_t now_ms);

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

// --- Touch (CST226SE direct I2C) ---

static bool touch_available = false;
static uint8_t touch_i2c_addr = 0x1A;  // Will be auto-detected (0x1A or 0x5A)

// Touch scroll state machine
enum TouchState { TOUCH_IDLE, TOUCH_ACTIVE };
static TouchState touch_state = TOUCH_IDLE;
static int16_t touch_start_x = 0;
static int16_t touch_start_y = 0;
static int16_t touch_last_x = 0;
static int16_t touch_last_y = 0;
static unsigned long touch_start_ms = 0;
static bool touch_did_scroll = false;
static unsigned long last_touch_poll = 0;
static constexpr unsigned long TOUCH_POLL_INTERVAL_MS = 90;
static constexpr int16_t TOUCH_SCROLL_THRESHOLD = 16; // 2 text lines worth of pixels
static constexpr unsigned long TOUCH_TAP_MAX_MS = 300;
static constexpr int16_t TOUCH_TAP_DEADZONE_X = SCREEN_W / 8;
static constexpr int16_t TOUCH_TAP_DEADZONE_Y = (SCREEN_H - STATUS_H) / 8;
static constexpr int TOUCH_SCROLL_MAX_STEPS_PER_POLL = 3;
static constexpr int16_t BT_TOUCH_TAP_MOVE_MAX = 10;
static constexpr int BT_TOUCH_MOVE_DIVISOR = 2;

// Read 7 bytes from CST226SE register 0xD000 (touch data report)
// Returns: 1=touched (x/y set), 0=not touched, -1=I2C failure
static int cst226ReadTouch(int16_t* x, int16_t* y) {
    // Write 2-byte register address 0xD000
    Wire.beginTransmission(touch_i2c_addr);
    Wire.write((uint8_t)0xD0);
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission() != 0) return -1;

    Wire.requestFrom(touch_i2c_addr, (uint8_t)7);
    if (Wire.available() < 7) {
        while (Wire.available()) Wire.read();
        return -1;
    }

    uint8_t data[7];
    for (int i = 0; i < 7; i++) data[i] = Wire.read();

    // data[0]: finger1 ID/state (state in low nibble, 0x06 = pressed)
    // data[1]: X high 8 bits, data[2]: Y high 8 bits
    // data[3]: X low 4 | Y low 4, data[4]: pressure
    bool touched = ((data[0] & 0x0F) == 0x06);
    if (touched) {
        if (x) *x = (int16_t)((data[1] << 4) | ((data[3] >> 4) & 0x0F));
        if (y) *y = (int16_t)((data[2] << 4) | (data[3] & 0x0F));
    }
    return touched ? 1 : 0;
}

// --- App Mode ---

enum AppMode { MODE_NOTEPAD, MODE_TERMINAL, MODE_COMMAND, MODE_BT };
static volatile AppMode app_mode = MODE_NOTEPAD;

// --- Editor State (shared between cores, protected by mutex) ---

// --- Command Processor State ---
#define CMD_BUF_LEN 64
static char cmd_buf[CMD_BUF_LEN + 1];
static int  cmd_len = 0;
static AppMode cmd_return_mode = MODE_NOTEPAD;  // Mode to return to after command
static constexpr int CMD_HISTORY_CAP = 20;
static char cmd_history[CMD_HISTORY_CAP][CMD_BUF_LEN + 1];
static int  cmd_history_count = 0;
static int  cmd_history_nav = -1; // -1 = editing live buffer, otherwise history index
static char cmd_history_draft[CMD_BUF_LEN + 1];

// Multi-line command result (half screen)
#define CMD_RESULT_LINES 13
static char cmd_result[CMD_RESULT_LINES][COLS_PER_LINE + 1];
static int  cmd_result_count = 0;
static bool cmd_result_valid = false;

// --- File State ---
static String current_file = "";
static bool file_modified = false;

static SemaphoreHandle_t state_mutex;
// Protects libssh channel I/O across cores (receive task vs input writers).
static SemaphoreHandle_t ssh_io_mutex;
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
static unsigned long terminal_last_ctrl_c_ms = 0;

// Display task snapshot — private to core 0
static char snap_buf[MAX_TEXT_LEN + 1];
static int  snap_len       = 0;
static int  snap_cursor    = 0;
static int  snap_scroll    = 0;
static bool snap_shift     = false;
static bool snap_sym       = false;

enum TouchTapArrow {
    TOUCH_TAP_ARROW_NONE,
    TOUCH_TAP_ARROW_UP,
    TOUCH_TAP_ARROW_DOWN,
    TOUCH_TAP_ARROW_LEFT,
    TOUCH_TAP_ARROW_RIGHT,
};

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
// Mouse reporting modes requested by remote terminal app (DECSET private modes).
static uint8_t term_mouse_tracking_mode_mask = 0; // ?1000/?1002/?1003
static bool term_mouse_sgr_mode = false;          // ?1006

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
static bool term_wrap_pending = false; // delayed wrap after writing at last column

// UTF-8 parsing state
static int utf8_remaining = 0;
static uint32_t utf8_codepoint = 0;

#if TDECK_AGENT_DEBUG
// Debug trace ring for SSH terminal parser input.
#define TERM_TRACE_CAP 4096
static uint8_t term_trace_buf[TERM_TRACE_CAP];
static uint16_t term_trace_head = 0;
static uint16_t term_trace_count = 0;
static uint32_t term_trace_total = 0;
static bool term_trace_enabled = false;

void terminalDebugTraceSet(bool enabled) {
    term_trace_enabled = enabled;
}

bool terminalDebugTraceEnabled() {
    return term_trace_enabled;
}

void terminalDebugTraceClear() {
    term_trace_head = 0;
    term_trace_count = 0;
    term_trace_total = 0;
}

void terminalDebugTraceRecord(uint8_t b) {
    if (!term_trace_enabled) return;
    term_trace_buf[term_trace_head] = b;
    term_trace_head = (uint16_t)((term_trace_head + 1) % TERM_TRACE_CAP);
    if (term_trace_count < TERM_TRACE_CAP) term_trace_count++;
    term_trace_total++;
}

void terminalDebugTraceDump(int max_bytes) {
    if (max_bytes <= 0 || max_bytes > TERM_TRACE_CAP) max_bytes = TERM_TRACE_CAP;
    int n = term_trace_count;
    if (n > max_bytes) n = max_bytes;
    int dropped = (int)term_trace_total - term_trace_count;

    SERIAL_LOGF("AGENT OK TRACE enabled=%d bytes=%d total=%lu dropped=%d\n",
                  term_trace_enabled ? 1 : 0, n, (unsigned long)term_trace_total, dropped);
    if (n <= 0) return;

    int start = term_trace_head - n;
    if (start < 0) start += TERM_TRACE_CAP;

    for (int i = 0; i < n; i += 16) {
        int chunk = (n - i > 16) ? 16 : (n - i);
        int idx = (start + i) % TERM_TRACE_CAP;
        SERIAL_LOGF("TRACEHEX %04d:", i);
        for (int j = 0; j < chunk; j++) {
            int pos = (idx + j) % TERM_TRACE_CAP;
            SERIAL_LOGF(" %02X", term_trace_buf[pos]);
        }
        SERIAL_LOG(" |");
        for (int j = 0; j < chunk; j++) {
            int pos = (idx + j) % TERM_TRACE_CAP;
            uint8_t c = term_trace_buf[pos];
            SERIAL_LOG((c >= 32 && c <= 126) ? (char)c : '.');
        }
        SERIAL_LOGLN("|");
    }
}

void terminalDebugStateDump() {
    SERIAL_LOGF(
        "AGENT OK TERMDBG row=%d col=%d scroll=%d lines=%d wrap=%d esc=%d csi=%d priv=%d utf8_rem=%d utf8_cp=%lu alt=%d sr_top=%d sr_bot=%d sr_set=%d\n",
        term_cursor_row, term_cursor_col, term_scroll, term_line_count,
        term_wrap_pending ? 1 : 0,
        in_escape ? 1 : 0, in_bracket ? 1 : 0, csi_private ? 1 : 0,
        utf8_remaining, (unsigned long)utf8_codepoint,
        term_alt_active ? 1 : 0, scroll_region_top, scroll_region_bot, scroll_region_set ? 1 : 0
    );
}
#else
void terminalDebugTraceSet(bool enabled) { (void)enabled; }
bool terminalDebugTraceEnabled() { return false; }
void terminalDebugTraceClear() {}
void terminalDebugTraceRecord(uint8_t b) { (void)b; }
void terminalDebugTraceDump(int max_bytes) { (void)max_bytes; }
void terminalDebugStateDump() {}
#endif

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

static void perfRecordRenderMs(uint32_t render_ms) {
    uint32_t now = millis();
    perfMaybeRollWindow(now);
    if (render_ms > perf_render_max5_ms) {
        perf_render_max5_ms = render_ms;
    }
}

static void perfMaybeRollWindow(uint32_t now_ms) {
    uint32_t start_ms = perf_window_start_ms;
    if (start_ms == 0 || now_ms - start_ms >= PERF_WINDOW_MS) {
        perf_window_start_ms = now_ms;
        perf_render_max5_ms = 0;
        perf_loop_max5_ms = 0;
        perf_heap_min5_kb = ESP.getFreeHeap() / 1024U;
    }
}

static void perfLoopTick() {
    static unsigned long last_loop_tick_ms = 0;
    static unsigned long last_heap_sample_ms = 0;
    unsigned long now = millis();
    perfMaybeRollWindow(now);

    if (last_loop_tick_ms != 0) {
        uint32_t loop_dt = now - last_loop_tick_ms;
        if (loop_dt > perf_loop_max5_ms) {
            perf_loop_max5_ms = loop_dt;
        }
    }
    last_loop_tick_ms = now;

    if (perf_heap_min5_kb == 0 || now - last_heap_sample_ms >= 200) {
        uint32_t free_kb = ESP.getFreeHeap() / 1024U;
        if (perf_heap_min5_kb == 0 || free_kb < perf_heap_min5_kb) {
            perf_heap_min5_kb = free_kb;
        }
        last_heap_sample_ms = now;
    }
}

static void buildPerfStatusCompact(char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    perfMaybeRollWindow(millis());
    uint32_t min_heap_kb = perf_heap_min5_kb;
    if (min_heap_kb == 0) min_heap_kb = ESP.getFreeHeap() / 1024U;

    snprintf(out, out_len, "H%lu R%lu L%lu",
             (unsigned long)min_heap_kb,
             (unsigned long)perf_render_max5_ms,
             (unsigned long)perf_loop_max5_ms);
}

// --- Terminal Buffer Operations ---

static uint8_t termMouseTrackingModeBit(int mode) {
    switch (mode) {
        case 1000: return 0x01; // button-event tracking
        case 1002: return 0x02; // button-motion tracking
        case 1003: return 0x04; // any-motion tracking
        default:   return 0x00;
    }
}

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
    term_mouse_tracking_mode_mask = 0;
    term_mouse_sgr_mode = false;
    scroll_region_top = 0;
    scroll_region_bot = ROWS_PER_SCREEN - 1;
    scroll_region_set = false;
    term_alt_active = false;
    cursor_visible = true;
    term_wrap_pending = false;
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
    term_wrap_pending = false;
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
    term_wrap_pending = false;
    scroll_region_top = 0;
    scroll_region_bot = ROWS_PER_SCREEN - 1;
    scroll_region_set = false;
    term_alt_active = false;
}

void handleCSI(char final_char) {
    int p0 = (csi_param_count > 0) ? csi_params[0] : 0;
    int p1 = (csi_param_count > 1) ? csi_params[1] : 0;
    int max_row = term_alt_active ? ROWS_PER_SCREEN : TERM_ROWS;

    // Formatting-only CSI (SGR) should not consume delayed-wrap state.
    if (final_char != 'm') term_wrap_pending = false;

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
                    case 1000:
                    case 1002:
                    case 1003:
                        term_mouse_tracking_mode_mask |= termMouseTrackingModeBit(csi_params[pi]);
                        break;
                    case 1006:
                        term_mouse_sgr_mode = true;
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
                    case 1000:
                    case 1002:
                    case 1003:
                        term_mouse_tracking_mode_mask &= (uint8_t)~termMouseTrackingModeBit(csi_params[pi]);
                        break;
                    case 1006:
                        term_mouse_sgr_mode = false;
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
    if (term_wrap_pending) {
        term_cursor_col = 0;
        terminalCursorDown();
        term_wrap_pending = false;
    }

    term_buf[term_cursor_row][term_cursor_col] = ch;
    if (term_cursor_col >= TERM_COLS - 1) {
        term_wrap_pending = true;
    } else {
        term_cursor_col++;
    }
}

void terminalAppendOutput(const char* data, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        terminalDebugTraceRecord(c);

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
                        term_wrap_pending = false;
                        if (term_cursor_row == scroll_region_top) {
                            terminalScrollRegionDown(scroll_region_top, scroll_region_bot);
                        } else if (term_cursor_row > 0) {
                            term_cursor_row--;
                        }
                        break;
                    case 'D': // Index — cursor down, scroll region up if at bottom
                        term_wrap_pending = false;
                        if (term_cursor_row == scroll_region_bot) {
                            terminalScrollRegionUp(scroll_region_top, scroll_region_bot);
                        } else {
                            terminalCursorDown();
                        }
                        break;
                    case 'E': // Next Line — cursor to start of next line
                        term_wrap_pending = false;
                        term_cursor_col = 0;
                        terminalCursorDown();
                        break;
                    case '7': // Save cursor
                        saved_cursor_row = term_cursor_row;
                        saved_cursor_col = term_cursor_col;
                        break;
                    case '8': // Restore cursor
                        term_wrap_pending = false;
                        term_cursor_row = saved_cursor_row;
                        term_cursor_col = saved_cursor_col;
                        break;
                    case 'c': // Full reset
                        term_wrap_pending = false;
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
            term_wrap_pending = false;
            term_cursor_col = 0;
            terminalCursorDown();
            continue;
        }

        if (c == '\r') {
            term_wrap_pending = false;
            term_cursor_col = 0;
            continue;
        }

        if (c == '\b' || c == 0x7F) {
            if (term_wrap_pending) term_wrap_pending = false;
            if (term_cursor_col > 0) {
                term_cursor_col--;
                term_buf[term_cursor_row][term_cursor_col] = ' ';
            }
            continue;
        }

        if (c == '\t') {
            if (term_wrap_pending) term_wrap_pending = false;
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
        SERIAL_LOGF("SD: mounted, size=%lluMB\n", SD.cardSize() / (1024 * 1024));
    } else {
        sd_mounted = false;
        SERIAL_LOGLN("SD: mount failed");
    }
}

void sdLoadConfig() {
    if (!sd_mounted) return;
    File f = SD.open("/CONFIG", FILE_READ);
    if (!f) { SERIAL_LOGLN("SD: no /CONFIG"); return; }

    // CONFIG format: section-based, # comments, blank lines skipped
    // # wifi — pairs of ssid/password (variable count)
    // # ssh — host, port, user, pass, [optional vpn-host]
    // # vpn — ENABLE, privkey, pubkey, psk, ip, endpoint, port, [optional dns]
    // # bt   — optional BLE settings ([optional enable], name, [optional 6-digit pin])
    // # time — optional timezone (POSIX TZ string), e.g. PST8PDT,M3.2.0,M11.1.0
    // # msh  — optional channel config (line1=name, line2=key spec)
    enum { SEC_WIFI, SEC_SSH, SEC_VPN, SEC_BT, SEC_TIME, SEC_MSH } section = SEC_WIFI;
    int field = 0;  // field index within current section
    bool wifi_expect_ssid = true;
    char wifi_ssid[64] = "";

    auto addWiFiAP = [&](const char* ssid, const char* pass) {
        if (config_wifi_count >= MAX_WIFI_APS) return;
        strncpy(config_wifi[config_wifi_count].ssid, ssid, 63);
        config_wifi[config_wifi_count].ssid[63] = '\0';
        strncpy(config_wifi[config_wifi_count].pass, pass, 63);
        config_wifi[config_wifi_count].pass[63] = '\0';
        SERIAL_LOGF("SD: WiFi AP added: %s%s\n",
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
            } else if (line.indexOf("time") >= 0) {
                section = SEC_TIME;
                field = 0;
            } else if (line.indexOf("msh") >= 0 || line.indexOf("meshtastic") >= 0) {
                section = SEC_MSH;
                field = 0;
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
            // line 2+ (optional): legacy lines ignored
            // Note: runtime always starts with BT off; enable/passkey flags are ignored.
            if (field == 0) {
                String lowered = line;
                lowered.toLowerCase();
                if (lowered == "enable" || lowered == "enabled" || lowered == "on" || lowered == "true" || lowered == "1") {
                    SERIAL_LOGLN("SD: BT enable flag ignored (runtime toggle only)");
                    continue;
                }
                if (lowered == "disable" || lowered == "disabled" || lowered == "off" || lowered == "false" || lowered == "0") {
                    SERIAL_LOGLN("SD: BT disable flag ignored (runtime toggle only)");
                    continue;
                }
                setBtName(line);
                field = 1;
                continue;
            } else if (field == 1) {
                SERIAL_LOGF("SD: BT legacy line ignored: %s\n", line.c_str());
                field++;
                continue;
            }
            field++;
        } else if (section == SEC_TIME) {
            if (field == 0) {
                strncpy(config_time_tz, line.c_str(), sizeof(config_time_tz) - 1);
                config_time_tz[sizeof(config_time_tz) - 1] = '\0';
                SERIAL_LOGF("SD: timezone set: %s\n", config_time_tz);
            }
            field++;
        } else if (section == SEC_MSH) {
            String lowered = line;
            lowered.toLowerCase();

            if (lowered.startsWith("name=")) {
                String value = line.substring(5);
                value.trim();
                strncpy(config_msh_channel, value.c_str(), sizeof(config_msh_channel) - 1);
                config_msh_channel[sizeof(config_msh_channel) - 1] = '\0';
                field = 1;
                continue;
            }
            if (lowered.startsWith("psk=") || lowered.startsWith("key=")) {
                int eq = line.indexOf('=');
                String value = (eq >= 0) ? line.substring(eq + 1) : "";
                value.trim();
                strncpy(config_msh_psk, value.c_str(), sizeof(config_msh_psk) - 1);
                config_msh_psk[sizeof(config_msh_psk) - 1] = '\0';
                field = 2;
                continue;
            }

            if (field == 0) {
                strncpy(config_msh_channel, line.c_str(), sizeof(config_msh_channel) - 1);
                config_msh_channel[sizeof(config_msh_channel) - 1] = '\0';
            } else if (field == 1) {
                strncpy(config_msh_psk, line.c_str(), sizeof(config_msh_psk) - 1);
                config_msh_psk[sizeof(config_msh_psk) - 1] = '\0';
            }
            field++;
        }
    }
    flushPendingOpenWiFi();
    f.close();
    SERIAL_LOGF("SD: config loaded (%d WiFi APs, host=%s, VPN=%s, BT=runtime, BT name=%s, TZ=%s, MSH=%s)\n",
                  config_wifi_count,
                  config_ssh_host,
                  vpnConfigured() ? "yes" : "no",
                  config_bt_name,
                  config_time_tz,
                  config_msh_channel[0] ? config_msh_channel : "(default)");
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
enum CmdPickerMode {
    CMD_PICKER_NONE = 0,
    CMD_PICKER_EDIT,
    CMD_PICKER_WIFI,
};

struct WifiScanPickerEntry {
    char ssid[64];
    int rssi;
    bool known;
    bool open_network;
};

static constexpr int WIFI_SCAN_PICKER_MAX = 32;
static WifiScanPickerEntry wifi_scan_picker[WIFI_SCAN_PICKER_MAX];
static int wifi_scan_picker_count = 0;

static CmdPickerMode cmd_picker_mode = CMD_PICKER_NONE;
static int  cmd_picker_indices[MAX_FILE_LIST];
static int  cmd_picker_count = 0;
static int  cmd_picker_selected = 0;
static int  cmd_picker_top = 0;

// Implemented in cli_module.hpp.
bool wifiPickerConnectSelectedNetwork(const char* ssid, bool open_network, bool known_network);

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

// Caller must hold state_mutex.
static void cmdSetBufferLocked(const char* src) {
    if (!src) src = "";
    strncpy(cmd_buf, src, CMD_BUF_LEN);
    cmd_buf[CMD_BUF_LEN] = '\0';
    cmd_len = (int)strlen(cmd_buf);
}

// Caller must hold state_mutex.
static void cmdHistoryResetBrowseLocked() {
    cmd_history_nav = -1;
    cmd_history_draft[0] = '\0';
}

// Caller must hold state_mutex.
static void cmdHistoryAddLocked(const char* cmd) {
    if (!cmd) return;

    const char* p = cmd;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return; // Ignore empty commands.

    if (cmd_history_count > 0) {
        if (strcmp(cmd_history[cmd_history_count - 1], cmd) == 0) {
            cmdHistoryResetBrowseLocked();
            return;
        }
    }

    if (cmd_history_count < CMD_HISTORY_CAP) {
        strncpy(cmd_history[cmd_history_count], cmd, CMD_BUF_LEN);
        cmd_history[cmd_history_count][CMD_BUF_LEN] = '\0';
        cmd_history_count++;
    } else {
        for (int i = 1; i < CMD_HISTORY_CAP; i++) {
            memcpy(cmd_history[i - 1], cmd_history[i], CMD_BUF_LEN + 1);
        }
        strncpy(cmd_history[CMD_HISTORY_CAP - 1], cmd, CMD_BUF_LEN);
        cmd_history[CMD_HISTORY_CAP - 1][CMD_BUF_LEN] = '\0';
        cmd_history_count = CMD_HISTORY_CAP;
    }

    cmdHistoryResetBrowseLocked();
}

// Caller must hold state_mutex.
// `direction` is -1 for older command, +1 for newer command.
static bool cmdHistoryBrowseLocked(int direction) {
    if (cmd_history_count <= 0) return false;
    if (direction != -1 && direction != 1) return false;

    if (direction == -1) {
        if (cmd_history_nav < 0) {
            strncpy(cmd_history_draft, cmd_buf, CMD_BUF_LEN);
            cmd_history_draft[CMD_BUF_LEN] = '\0';
            cmd_history_nav = cmd_history_count - 1;
        } else if (cmd_history_nav > 0) {
            cmd_history_nav--;
        } else {
            return false;
        }
        cmdSetBufferLocked(cmd_history[cmd_history_nav]);
        return true;
    }

    if (cmd_history_nav < 0) return false;
    if (cmd_history_nav < cmd_history_count - 1) {
        cmd_history_nav++;
        cmdSetBufferLocked(cmd_history[cmd_history_nav]);
    } else {
        cmd_history_nav = -1;
        cmdSetBufferLocked(cmd_history_draft);
    }
    return true;
}

bool cmdPickerIsActive() {
    return cmd_picker_mode != CMD_PICKER_NONE && cmd_picker_count > 0;
}

bool cmdPickerIsEditActive() {
    return cmd_picker_mode == CMD_PICKER_EDIT && cmdPickerIsActive();
}

bool cmdPickerIsWifiActive() {
    return cmd_picker_mode == CMD_PICKER_WIFI && cmdPickerIsActive();
}

const char* cmdPickerPromptWord() {
    if (cmd_picker_mode == CMD_PICKER_EDIT) return "edit";
    if (cmd_picker_mode == CMD_PICKER_WIFI) return "ws";
    return "";
}

const char* cmdPickerStatusHint() {
    if (cmd_picker_mode == CMD_PICKER_WIFI) return "[PICK] WiFi W/S A/D Enter";
    if (cmd_picker_mode == CMD_PICKER_EDIT) return "[PICK] Edit W/S A/D Enter";
    return "";
}

int cmdPickerVisibleRows() {
    int rows = CMD_RESULT_LINES - 1; // Reserve first line for picker status/help text
    if (rows < 1) rows = 1;
    return rows;
}

void cmdPickerStop() {
    cmd_picker_mode = CMD_PICKER_NONE;
    cmd_picker_count = 0;
    cmd_picker_selected = 0;
    cmd_picker_top = 0;
}

void cmdPickerSyncViewport() {
    if (cmd_picker_count <= 0) {
        cmd_picker_top = 0;
        cmd_picker_selected = 0;
        return;
    }
    if (cmd_picker_selected < 0) cmd_picker_selected = 0;
    if (cmd_picker_selected >= cmd_picker_count) cmd_picker_selected = cmd_picker_count - 1;

    int rows = cmdPickerVisibleRows();
    if (cmd_picker_selected < cmd_picker_top) {
        cmd_picker_top = cmd_picker_selected;
    } else if (cmd_picker_selected >= cmd_picker_top + rows) {
        cmd_picker_top = cmd_picker_selected - rows + 1;
    }

    int max_top = cmd_picker_count - rows;
    if (max_top < 0) max_top = 0;
    if (cmd_picker_top < 0) cmd_picker_top = 0;
    if (cmd_picker_top > max_top) cmd_picker_top = max_top;
}

void cmdPickerRender() {
    if (!cmdPickerIsActive()) {
        if (cmd_picker_mode == CMD_PICKER_WIFI) cmdSetResult("(no wifi)");
        else cmdSetResult("(no files)");
        return;
    }

    cmdPickerSyncViewport();
    cmdClearResult();

    if (cmd_picker_mode == CMD_PICKER_EDIT) {
        cmdAddLine("Edit %d/%d W/S A/D Enter", cmd_picker_selected + 1, cmd_picker_count);
    } else if (cmd_picker_mode == CMD_PICKER_WIFI) {
        cmdAddLine("WiFi %d/%d *known o=open", cmd_picker_selected + 1, cmd_picker_count);
    } else {
        cmdSetResult("(picker unavailable)");
        return;
    }

    int rows = cmdPickerVisibleRows();
    for (int i = 0; i < rows && cmd_result_count < CMD_RESULT_LINES; i++) {
        int list_idx = cmd_picker_top + i;
        if (list_idx >= cmd_picker_count) break;
        int ref_idx = cmd_picker_indices[list_idx];

        if (cmd_picker_mode == CMD_PICKER_EDIT) {
            if (ref_idx < 0 || ref_idx >= MAX_FILE_LIST) continue;
            const FileEntry& entry = file_list[ref_idx];
            cmdAddLine("%c%s %dB", (list_idx == cmd_picker_selected) ? '>' : ' ', entry.name, (int)entry.size);
        } else if (cmd_picker_mode == CMD_PICKER_WIFI) {
            if (ref_idx < 0 || ref_idx >= wifi_scan_picker_count) continue;
            const WifiScanPickerEntry& entry = wifi_scan_picker[ref_idx];
            char known_mark = entry.known ? '*' : ' ';
            char open_mark = entry.open_network ? 'o' : 'l';
            cmdAddLine("%c%c%c %s %ddBm",
                       (list_idx == cmd_picker_selected) ? '>' : ' ',
                       known_mark,
                       open_mark,
                       entry.ssid,
                       entry.rssi);
        }
    }
}

void cmdPickerStart(CmdPickerMode mode) {
    cmd_picker_mode = mode;
    cmd_picker_selected = 0;
    cmd_picker_top = 0;
    cmdPickerRender();
}

bool cmdPickerMoveSelection(int delta) {
    if (!cmdPickerIsActive()) return false;
    int next = cmd_picker_selected + delta;
    if (next < 0) next = 0;
    if (next >= cmd_picker_count) next = cmd_picker_count - 1;
    if (next == cmd_picker_selected) return false;
    cmd_picker_selected = next;
    cmdPickerRender();
    return true;
}

bool cmdPickerPage(int direction) {
    int rows = cmdPickerVisibleRows();
    if (rows < 1) rows = 1;
    return cmdPickerMoveSelection(direction * rows);
}

bool cmdEditPickerOpenSelectedInternal() {
    if (!cmdPickerIsEditActive()) return false;

    int list_idx = cmd_picker_selected;
    if (list_idx < 0 || list_idx >= cmd_picker_count) return false;
    int file_idx = cmd_picker_indices[list_idx];
    if (file_idx < 0 || file_idx >= MAX_FILE_LIST) return false;
    const FileEntry& entry = file_list[file_idx];

    autoSaveDirty();
    String path = "/" + String(entry.name);
    bool ok = loadFromFile(path.c_str());
    cmdPickerStop();
    if (ok) {
        current_file = path;
        cmdSetResult("Loaded %s (%d B)", entry.name, text_len);
        app_mode = MODE_NOTEPAD;
    } else {
        cmdSetResult("Load failed: %s", entry.name);
    }
    return true;
}

bool cmdWifiPickerOpenSelectedInternal() {
    if (!cmdPickerIsWifiActive()) return false;

    int list_idx = cmd_picker_selected;
    if (list_idx < 0 || list_idx >= cmd_picker_count) return false;
    int wifi_idx = cmd_picker_indices[list_idx];
    if (wifi_idx < 0 || wifi_idx >= wifi_scan_picker_count) return false;

    char ssid[sizeof(wifi_scan_picker[wifi_idx].ssid)];
    strncpy(ssid, wifi_scan_picker[wifi_idx].ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    bool open_network = wifi_scan_picker[wifi_idx].open_network;
    bool known_network = wifi_scan_picker[wifi_idx].known;

    cmdPickerStop();
    return wifiPickerConnectSelectedNetwork(ssid, open_network, known_network);
}

bool cmdPickerOpenSelected() {
    if (!cmdPickerIsActive()) return false;
    if (cmd_picker_mode == CMD_PICKER_EDIT) return cmdEditPickerOpenSelectedInternal();
    if (cmd_picker_mode == CMD_PICKER_WIFI) return cmdWifiPickerOpenSelectedInternal();
    return false;
}

bool cmdPickerCancel() {
    if (!cmdPickerIsActive()) return false;
    CmdPickerMode mode = cmd_picker_mode;
    cmdPickerStop();
    if (mode == CMD_PICKER_WIFI) cmdSetResult("WiFi select cancelled");
    else cmdSetResult("Edit cancelled");
    return true;
}

void cmdEditPickerStop() {
    cmdPickerStop();
}

bool cmdEditPickerStart() {
    int n = listDirectory("/");
    if (n < 0) {
        cmdPickerStop();
        cmdSetResult("Can't read SD");
        return false;
    }

    cmd_picker_count = 0;
    for (int i = 0; i < n && i < MAX_FILE_LIST; i++) {
        if (!file_list[i].is_dir) {
            cmd_picker_indices[cmd_picker_count++] = i;
        }
    }

    if (cmd_picker_count == 0) {
        cmdPickerStop();
        cmdSetResult("(no files)");
        return false;
    }

    cmdPickerStart(CMD_PICKER_EDIT);
    return true;
}

bool cmdEditPickerMoveSelection(int delta) {
    if (!cmdPickerIsEditActive()) return false;
    return cmdPickerMoveSelection(delta);
}

bool cmdEditPickerPage(int direction) {
    if (!cmdPickerIsEditActive()) return false;
    return cmdPickerPage(direction);
}

bool cmdEditPickerOpenSelected() {
    if (!cmdPickerIsEditActive()) return false;
    return cmdEditPickerOpenSelectedInternal();
}

void cmdWifiPickerResetResults() {
    wifi_scan_picker_count = 0;
}

bool cmdWifiPickerAddResult(const char* ssid, int rssi, bool known, bool open_network) {
    if (!ssid || ssid[0] == '\0') return false;

    // Collapse duplicate SSIDs and keep strongest RSSI.
    for (int i = 0; i < wifi_scan_picker_count; i++) {
        if (strncmp(wifi_scan_picker[i].ssid, ssid, sizeof(wifi_scan_picker[i].ssid)) == 0) {
            if (rssi > wifi_scan_picker[i].rssi) wifi_scan_picker[i].rssi = rssi;
            wifi_scan_picker[i].known = wifi_scan_picker[i].known || known;
            wifi_scan_picker[i].open_network = wifi_scan_picker[i].open_network || open_network;
            return true;
        }
    }

    if (wifi_scan_picker_count >= WIFI_SCAN_PICKER_MAX) return false;
    WifiScanPickerEntry& entry = wifi_scan_picker[wifi_scan_picker_count++];
    strncpy(entry.ssid, ssid, sizeof(entry.ssid) - 1);
    entry.ssid[sizeof(entry.ssid) - 1] = '\0';
    entry.rssi = rssi;
    entry.known = known;
    entry.open_network = open_network;
    return true;
}

bool cmdWifiPickerStart() {
    if (wifi_scan_picker_count <= 0) {
        cmdPickerStop();
        cmdSetResult("No WiFi networks found");
        return false;
    }

    cmd_picker_count = 0;
    for (int i = 0; i < wifi_scan_picker_count && i < MAX_FILE_LIST; i++) {
        cmd_picker_indices[cmd_picker_count++] = i;
    }
    if (cmd_picker_count <= 0) {
        cmdPickerStop();
        cmdSetResult("No WiFi networks found");
        return false;
    }

    cmdPickerStart(CMD_PICKER_WIFI);
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

#include "time_sync_module.hpp"
#include "network_module.hpp"
#include "gnss_module.hpp"
#include "modem_module.hpp"
#include "meshtastic_module.hpp"
#include "bluetooth_module.hpp"
#include "screen_module.hpp"
#include "keyboard_module.hpp"
#include "cli_module.hpp"
#include "serial_agent_module.hpp"

// --- Setup & Loop ---

static bool terminalMouseTrackingEnabled() {
    return term_mouse_tracking_mode_mask != 0;
}

static void terminalSendMouseWheel(bool wheel_up) {
    // Use cursor location as event location so tmux can route wheel to active pane.
    int col = term_cursor_col;
    if (col < 0) col = 0;
    if (col >= TERM_COLS) col = TERM_COLS - 1;
    int x = col + 1; // 1-based

    int row = term_cursor_row - term_scroll;
    if (row < 0) row = 0;
    if (row >= ROWS_PER_SCREEN) row = ROWS_PER_SCREEN - 1;
    int y = row + 1; // 1-based

    if (term_mouse_sgr_mode) {
        char seq[32];
        int cb = wheel_up ? 64 : 65;
        int n = snprintf(seq, sizeof(seq), "\x1b[<%d;%d;%dM", cb, x, y);
        if (n > 0) sshSendString(seq, n);
        return;
    }

    // Legacy X10 protocol: ESC [ M Cb Cx Cy (each byte offset by 32).
    char seq[6];
    seq[0] = 0x1B;
    seq[1] = '[';
    seq[2] = 'M';
    seq[3] = (char)(32 + (wheel_up ? 64 : 65));
    seq[4] = (char)(32 + x);
    seq[5] = (char)(32 + y);
    sshSendString(seq, 6);
}

static void terminalSendArrowKey(TouchTapArrow arrow) {
    switch (arrow) {
        case TOUCH_TAP_ARROW_UP:    sshSendString("\x1b[A", 3); break;
        case TOUCH_TAP_ARROW_DOWN:  sshSendString("\x1b[B", 3); break;
        case TOUCH_TAP_ARROW_RIGHT: sshSendString("\x1b[C", 3); break;
        case TOUCH_TAP_ARROW_LEFT:  sshSendString("\x1b[D", 3); break;
        default: break;
    }
}

static void btSendArrowKey(TouchTapArrow arrow) {
    switch (arrow) {
        case TOUCH_TAP_ARROW_UP:    btSendUsage(0x52, 0); break;
        case TOUCH_TAP_ARROW_DOWN:  btSendUsage(0x51, 0); break;
        case TOUCH_TAP_ARROW_RIGHT: btSendUsage(0x4F, 0); break;
        case TOUCH_TAP_ARROW_LEFT:  btSendUsage(0x50, 0); break;
        default: break;
    }
}

static int8_t btClampMouseDelta(int value) {
    if (value > 127) return 127;
    if (value < -127) return -127;
    return (int8_t)value;
}

static void btTrackpadSendMove(int16_t raw_dx, int16_t raw_dy) {
    if (raw_dx == 0 && raw_dy == 0) return;
    int sx = raw_dx / BT_TOUCH_MOVE_DIVISOR;
    int sy = raw_dy / BT_TOUCH_MOVE_DIVISOR;
    if (sx == 0 && raw_dx != 0) sx = raw_dx > 0 ? 1 : -1;
    if (sy == 0 && raw_dy != 0) sy = raw_dy > 0 ? 1 : -1;
    btMouseMove(btClampMouseDelta(sx), btClampMouseDelta(sy), 0);
}

static TouchTapArrow touchTapArrowFromPoint(int16_t x, int16_t y) {
    const int touch_h = SCREEN_H - STATUS_H;
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= touch_h) {
        return TOUCH_TAP_ARROW_NONE;
    }

    int dx = x - (SCREEN_W / 2);
    int dy = y - (touch_h / 2);
    int adx = abs(dx);
    int ady = abs(dy);
    if (adx < TOUCH_TAP_DEADZONE_X && ady < TOUCH_TAP_DEADZONE_Y) {
        return TOUCH_TAP_ARROW_NONE;
    }

    if (adx >= ady) {
        return dx < 0 ? TOUCH_TAP_ARROW_LEFT : TOUCH_TAP_ARROW_RIGHT;
    }
    return dy < 0 ? TOUCH_TAP_ARROW_UP : TOUCH_TAP_ARROW_DOWN;
}

// Caller must hold state_mutex.
static void handleTouchArrowTapLocked(TouchTapArrow arrow) {
    if (arrow == TOUCH_TAP_ARROW_NONE) return;

    if (app_mode == MODE_NOTEPAD) {
        int old_cursor = cursor_pos;
        if (arrow == TOUCH_TAP_ARROW_UP) cursorUp();
        else if (arrow == TOUCH_TAP_ARROW_DOWN) cursorDown();
        else if (arrow == TOUCH_TAP_ARROW_LEFT) cursorLeft();
        else if (arrow == TOUCH_TAP_ARROW_RIGHT) cursorRight();
        if (cursor_pos != old_cursor) {
            render_requested = true;
        }
        return;
    }

    if (app_mode == MODE_TERMINAL) {
        terminalSendArrowKey(arrow);
        return;
    }

    if (app_mode == MODE_COMMAND) {
        bool changed = false;
        if (cmdPickerIsActive()) {
            if (arrow == TOUCH_TAP_ARROW_UP) changed = cmdPickerMoveSelection(-1);
            else if (arrow == TOUCH_TAP_ARROW_DOWN) changed = cmdPickerMoveSelection(1);
            else if (arrow == TOUCH_TAP_ARROW_LEFT) changed = cmdPickerPage(-1);
            else if (arrow == TOUCH_TAP_ARROW_RIGHT) changed = cmdPickerPage(1);
        } else {
            if (arrow == TOUCH_TAP_ARROW_UP) changed = cmdHistoryBrowseLocked(-1);
            else if (arrow == TOUCH_TAP_ARROW_DOWN) changed = cmdHistoryBrowseLocked(1);
        }
        if (changed) {
            render_requested = true;
        }
    }
}

void setup() {
    SERIAL_LOG_BEGIN(115200);
#if TDECK_AGENT_DEBUG
    delay(500);
#endif
    SERIAL_LOGLN("T-Deck Pro Notepad + Terminal starting...");
    timeSyncInit();

    // Clear any GPIO deep-sleep holds from a prior power-off cycle.
    gpio_hold_dis((gpio_num_t)BOARD_LORA_EN);
    gpio_hold_dis((gpio_num_t)BOARD_GPS_EN);
    gpio_hold_dis((gpio_num_t)BOARD_1V8_EN);
    gpio_hold_dis((gpio_num_t)BOARD_MODEM_POWER_EN);
    gpio_hold_dis((gpio_num_t)BOARD_MODEM_PWRKEY);
    gpio_hold_dis((gpio_num_t)BOARD_MODEM_DTR);
    gpio_hold_dis((gpio_num_t)BOARD_MODEM_RST);
    gpio_hold_dis((gpio_num_t)BOARD_KEYBOARD_LED);
    gpio_hold_dis((gpio_num_t)BOARD_LORA_CS);
    gpio_hold_dis((gpio_num_t)BOARD_LORA_RST);
    gpio_hold_dis((gpio_num_t)BOARD_SD_CS);
    gpio_hold_dis((gpio_num_t)BOARD_EPD_CS);
    gpio_hold_dis((gpio_num_t)BOARD_TOUCH_RST);
    gpio_deep_sleep_hold_dis();

    // BOOT is GPIO0 (active-low). Hold to trigger the same deep sleep path as "off".
    pinMode(BOARD_BOOT_PIN, INPUT_PULLUP);

    // Disable task watchdog — SSH blocking calls would trigger it
    esp_task_wdt_deinit();

    // Disable unused peripherals
    pinMode(BOARD_LORA_EN, OUTPUT);        digitalWrite(BOARD_LORA_EN, LOW);
    pinMode(BOARD_GPS_EN, OUTPUT);         digitalWrite(BOARD_GPS_EN, LOW);
    pinMode(BOARD_MODEM_POWER_EN, OUTPUT); digitalWrite(BOARD_MODEM_POWER_EN, LOW);
    pinMode(BOARD_MODEM_PWRKEY, OUTPUT);   digitalWrite(BOARD_MODEM_PWRKEY, LOW);
    pinMode(BOARD_MODEM_DTR, OUTPUT);      digitalWrite(BOARD_MODEM_DTR, LOW);
    pinMode(BOARD_MODEM_RST, OUTPUT);      digitalWrite(BOARD_MODEM_RST, HIGH);
    // Enable 1.8V rail early (powers touch controller)
    pinMode(BOARD_1V8_EN, OUTPUT);         digitalWrite(BOARD_1V8_EN, HIGH);
    // Keyboard backlight off
    pinMode(BOARD_KEYBOARD_LED, OUTPUT);
    digitalWrite(BOARD_KEYBOARD_LED, LOW);

    // SPI CS lines high
    pinMode(BOARD_LORA_CS, OUTPUT);  digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_LORA_RST, OUTPUT); digitalWrite(BOARD_LORA_RST, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);    digitalWrite(BOARD_SD_CS, HIGH);
    pinMode(BOARD_EPD_CS, OUTPUT);   digitalWrite(BOARD_EPD_CS, HIGH);

    // Hardware-reset touch controller BEFORE Wire.begin() so we don't
    // need to reinitialize I2C later (which would break TCA8418).
    // GPIO 45 is a strapping pin — toggle it before I2C is active.
    {
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << BOARD_TOUCH_RST);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
    }
    gpio_set_level((gpio_num_t)BOARD_TOUCH_RST, 0);
    delay(10);
    gpio_set_level((gpio_num_t)BOARD_TOUCH_RST, 1);
    delay(50);  // Wait for CST328 firmware to boot
    pinMode(BOARD_TOUCH_INT, INPUT_PULLUP);

    // Init I2C (after touch reset so GPIO 45 is stable)
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    Wire.setClock(200000);

    // Init keyboard
    if (!keypad.begin(0x34, &Wire)) {
        SERIAL_LOGLN("ERROR: TCA8418 keyboard not found!");
    } else {
        keypad.matrix(KEYPAD_ROWS, KEYPAD_COLS);
        keypad.flush();
        SERIAL_LOGLN("Keyboard OK");
    }

    // Init touch controller (CST328 at 0x1A)
    touch_available = false;
    Wire.beginTransmission(0x1A);
    uint8_t touch_ack = Wire.endTransmission();
    SERIAL_LOGF("Touch probe 0x1A: %d\n", touch_ack);

    if (touch_ack == 0) {
        touch_i2c_addr = 0x1A;

        // Enter command mode and read chip info
        Wire.beginTransmission(0x1A);
        Wire.write((uint8_t)0xD1);
        Wire.write((uint8_t)0x01);
        Wire.endTransmission();
        delay(10);

        Wire.beginTransmission(0x1A);
        Wire.write((uint8_t)0xD1);
        Wire.write((uint8_t)0xF4);
        if (Wire.endTransmission() == 0) {
            Wire.requestFrom((uint8_t)0x1A, (uint8_t)28);
            int avail = Wire.available();
            uint8_t buf[28] = {0};
            for (int i = 0; i < 28 && Wire.available(); i++) buf[i] = Wire.read();
            while (Wire.available()) Wire.read();
            if (avail >= 8) {
                SERIAL_LOGF("Touch: TX=%d RX=%d resX=%d resY=%d\n",
                    buf[0], buf[2], buf[4] | (buf[5] << 8), buf[6] | (buf[7] << 8));
            }
        }

        // Exit command mode → normal touch reporting
        Wire.beginTransmission(0x1A);
        Wire.write((uint8_t)0xD1);
        Wire.write((uint8_t)0x09);
        Wire.endTransmission();
        delay(5);

        touch_available = true;
        SERIAL_LOGLN("Touch OK");
    } else {
        SERIAL_LOGLN("Touch not found at 0x1A");
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
    timeSyncSetTimeZone(config_time_tz);
    gnssInit();
    modemInit();
    meshtasticInit();
    if (config_msh_channel[0] != '\0' || config_msh_psk[0] != '\0') {
        if (!meshtasticConfigureChannel(config_msh_channel[0] ? config_msh_channel : NULL,
                                        config_msh_psk[0] ? config_msh_psk : NULL)) {
            SERIAL_LOGF("MSH: config apply failed: %s\n", meshtasticLastError());
        } else {
            SERIAL_LOGF("MSH: config applied (channel=%s)\n", config_msh_channel[0] ? config_msh_channel : "LongFast");
        }
    }
    btInit();
    updateBattery();  // Seed battery_pct before the first status bar render.

    // Init terminal buffer
    terminalClear();

    // Create mutex
    state_mutex = xSemaphoreCreateMutex();
    ssh_io_mutex = xSemaphoreCreateMutex();

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

    SERIAL_LOGLN("Ready. Single-tap MIC for commands. Use `ssh` to open terminal.");
    SERIAL_LOGF("Free heap: %d bytes\n", ESP.getFreeHeap());
}

// Core 1: keyboard polling — never blocks on display
void loop() {
    agentPollSerial();
    perfLoopTick();
    modemPoll();
    wifiScanPoll();
    btScanPoll();
    gnssScanPoll();
    meshtasticScanPoll();
    meshtasticPoll();
    modemScanPoll();
    modemPowerNotifyPoll();
    gnssPoll();

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
        int prev_battery_pct = battery_pct;
        updateBattery();
        if (battery_pct != prev_battery_pct) {
            AppMode cur = app_mode;
            if (cur == MODE_TERMINAL) term_render_requested = true;
            else render_requested = true;
        }
    }

    // BLE maintenance: auto-advertise and reconnect handling.
    static unsigned long last_bt_check = 0;
    if (millis() - last_bt_check > 250) {
        last_bt_check = millis();
        btPoll();
    }

    // MIC single-tap timeout → open command processor
    if (mic_last_press > 0 && (millis() - mic_last_press >= MIC_CMD_TAP_DELAY_MS)) {
        mic_last_press = 0;
        if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(25)) == pdTRUE) {
            cmd_return_mode = app_mode;
            cmd_len = 0;
            cmd_buf[0] = '\0';
            cmdHistoryResetBrowseLocked();
            cmd_result_valid = false;
            cmdPickerStop();
            app_mode = MODE_COMMAND;
            xSemaphoreGive(state_mutex);
            render_requested = true;
        }
    }

    // --- Touch scroll polling ---
    if (touch_available && (millis() - last_touch_poll >= TOUCH_POLL_INTERVAL_MS)) {
        unsigned long now = millis();
        last_touch_poll = now;
        int16_t cur_x = 0;
        int16_t cur_y = 0;
        int touch_result = cst226ReadTouch(&cur_x, &cur_y);
        bool is_touched = (touch_result == 1);

        if (is_touched) {
            if (touch_state == TOUCH_IDLE) {
                // Touch just started
                touch_state = TOUCH_ACTIVE;
                touch_start_x = cur_x;
                touch_start_y = cur_y;
                touch_last_x = cur_x;
                touch_last_y = cur_y;
                touch_start_ms = now;
                touch_did_scroll = false;
            } else {
                AppMode mode = app_mode;
                if (mode == MODE_BT) {
                    int16_t raw_dx = cur_x - touch_last_x;
                    int16_t raw_dy = cur_y - touch_last_y;
                    btTrackpadSendMove(raw_dx, raw_dy);
                    touch_last_x = cur_x;
                    touch_last_y = cur_y;
                    if (raw_dx > BT_TOUCH_TAP_MOVE_MAX || raw_dx < -BT_TOUCH_TAP_MOVE_MAX ||
                        raw_dy > BT_TOUCH_TAP_MOVE_MAX || raw_dy < -BT_TOUCH_TAP_MOVE_MAX) {
                        touch_did_scroll = true;
                    }
                } else {
                    touch_last_x = cur_x;
                    touch_last_y = cur_y;
                    // Continuing touch — check for scroll gesture
                    int16_t delta_y = cur_y - touch_start_y;
                    if (delta_y > TOUCH_SCROLL_THRESHOLD || delta_y < -TOUCH_SCROLL_THRESHOLD) {
                        int lines_delta = delta_y / CHAR_H;
                        if (lines_delta != 0) {
                            if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                                if (mode == MODE_NOTEPAD) {
                                    // Natural scroll: finger down = see earlier content (scroll_line decreases)
                                    LayoutInfo li = computeLayoutFrom(text_buf, text_len, cursor_pos);
                                    int max_scroll = li.total_lines > ROWS_PER_SCREEN
                                        ? li.total_lines - ROWS_PER_SCREEN : 0;
                                    scroll_line -= lines_delta;
                                    if (scroll_line < 0) scroll_line = 0;
                                    if (scroll_line > max_scroll) scroll_line = max_scroll;
                                    render_requested = true;
                                } else if (mode == MODE_TERMINAL) {
                                    if (terminalMouseTrackingEnabled()) {
                                        int steps = lines_delta;
                                        if (steps > TOUCH_SCROLL_MAX_STEPS_PER_POLL) {
                                            steps = TOUCH_SCROLL_MAX_STEPS_PER_POLL;
                                        }
                                        if (steps < -TOUCH_SCROLL_MAX_STEPS_PER_POLL) {
                                            steps = -TOUCH_SCROLL_MAX_STEPS_PER_POLL;
                                        }
                                        if (steps > 0) {
                                            for (int i = 0; i < steps; i++) {
                                                terminalSendMouseWheel(true);
                                            }
                                        } else {
                                            for (int i = 0; i < -steps; i++) {
                                                terminalSendMouseWheel(false);
                                            }
                                        }
                                    } else {
                                        int max_scroll = term_line_count > ROWS_PER_SCREEN
                                            ? term_line_count - ROWS_PER_SCREEN : 0;
                                        term_scroll -= lines_delta;
                                        if (term_scroll < 0) term_scroll = 0;
                                        if (term_scroll > max_scroll) term_scroll = max_scroll;
                                        term_render_requested = true;
                                    }
                                }
                                xSemaphoreGive(state_mutex);
                            }
                            touch_start_y = cur_y;
                            touch_did_scroll = true;
                        }
                    }
                }
            }
        } else {
            if (touch_state == TOUCH_ACTIVE) {
                unsigned long tap_ms = now - touch_start_ms;
                int16_t move_x = touch_last_x - touch_start_x;
                int16_t move_y = touch_last_y - touch_start_y;
                AppMode mode = app_mode;
                if (mode == MODE_BT) {
                    bool moved_too_far = (move_x > BT_TOUCH_TAP_MOVE_MAX || move_x < -BT_TOUCH_TAP_MOVE_MAX
                        || move_y > BT_TOUCH_TAP_MOVE_MAX || move_y < -BT_TOUCH_TAP_MOVE_MAX);
                    if (tap_ms <= TOUCH_TAP_MAX_MS && !moved_too_far) {
                        TouchTapArrow arrow = touchTapArrowFromPoint(touch_last_x, touch_last_y);
                        if (arrow != TOUCH_TAP_ARROW_NONE) {
                            btSendArrowKey(arrow);
                        } else {
                            btMouseClick(1);
                        }
                    }
                } else if (!touch_did_scroll) {
                    bool moved_too_far = (move_x > TOUCH_SCROLL_THRESHOLD || move_x < -TOUCH_SCROLL_THRESHOLD
                        || move_y > TOUCH_SCROLL_THRESHOLD || move_y < -TOUCH_SCROLL_THRESHOLD);
                    if (tap_ms <= TOUCH_TAP_MAX_MS && !moved_too_far) {
                        TouchTapArrow arrow = touchTapArrowFromPoint(touch_last_x, touch_last_y);
                        if (arrow != TOUCH_TAP_ARROW_NONE) {
                            if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                                handleTouchArrowTapLocked(arrow);
                                xSemaphoreGive(state_mutex);
                            }
                        }
                    }
                }
            }
            touch_state = TOUCH_IDLE;
            touch_did_scroll = false;
        }
    }

    while (keypad.available() > 0) {
        int ev = keypad.getEvent();
        if (!(ev & 0x80)) continue;  // skip release events

        if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(25)) != pdTRUE) {
            continue;
        }
        bool needs_render = false;
        AppMode mode = app_mode;
        if (mode == MODE_NOTEPAD) {
            needs_render = handleNotepadKeyPress(ev);
        } else if (mode == MODE_TERMINAL) {
            needs_render = handleTerminalKeyPress(ev);
        } else if (mode == MODE_BT) {
            needs_render = handleBluetoothKeyPress(ev);
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
            } else if (cur == MODE_COMMAND || cur == MODE_BT) {
                render_requested = true;
            }
        }
    }
}
