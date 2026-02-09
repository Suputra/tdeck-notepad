#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <GxEPD2_BW.h>
#include <Adafruit_TCA8418.h>
#include <WiFi.h>
#include <libssh/libssh.h>
#include <esp_task_wdt.h>
#include <esp_netif.h>
#include <driver/uart.h>

// --- WiFi / SSH Credentials ---

#define WIFI_SSID     "REDACTED_SSID"
#define WIFI_PASSWORD "REDACTED_WIFI_PASS"

#define SSH_HOST      "REDACTED_HOST"
#define SSH_PORT      22
#define SSH_USER      "saahas"
#define SSH_PASS      "REDACTED_SSH_PASS"

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

// --- Command Processor State ---
#define CMD_BUF_LEN 64
static char cmd_buf[CMD_BUF_LEN + 1];
static int  cmd_len = 0;
static char cmd_result[128];  // Result message to display
static bool cmd_result_valid = false;
static AppMode cmd_return_mode = MODE_NOTEPAD;  // Mode to return to after command

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

static SemaphoreHandle_t state_mutex;
static volatile bool render_requested = false;

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
}

void terminalScrollUp() {
    // Shift all lines up by one, clear the last line
    for (int i = 0; i < TERM_ROWS - 1; i++) {
        memcpy(term_buf[i], term_buf[i + 1], TERM_COLS + 1);
    }
    memset(term_buf[TERM_ROWS - 1], ' ', TERM_COLS);
    term_buf[TERM_ROWS - 1][TERM_COLS] = '\0';
    if (term_cursor_row > 0) term_cursor_row--;
    if (term_line_count > 1) term_line_count--;
}

void handleCSI(char final_char) {
    int p0 = (csi_param_count > 0) ? csi_params[0] : 0;
    int p1 = (csi_param_count > 1) ? csi_params[1] : 0;

    switch (final_char) {
        case 'A': // Cursor Up
            term_cursor_row -= (p0 > 0) ? p0 : 1;
            if (term_cursor_row < 0) term_cursor_row = 0;
            break;
        case 'B': // Cursor Down
            term_cursor_row += (p0 > 0) ? p0 : 1;
            if (term_cursor_row >= TERM_ROWS) term_cursor_row = TERM_ROWS - 1;
            break;
        case 'C': // Cursor Right
            term_cursor_col += (p0 > 0) ? p0 : 1;
            if (term_cursor_col >= TERM_COLS) term_cursor_col = TERM_COLS - 1;
            break;
        case 'D': // Cursor Left
            term_cursor_col -= (p0 > 0) ? p0 : 1;
            if (term_cursor_col < 0) term_cursor_col = 0;
            break;
        case 'H': // Cursor Position (row;col) — 1-based
        case 'f': // Same as H
            term_cursor_row = (p0 > 0) ? p0 - 1 : 0;
            term_cursor_col = (p1 > 0) ? p1 - 1 : 0;
            if (term_cursor_row >= TERM_ROWS) term_cursor_row = TERM_ROWS - 1;
            if (term_cursor_col >= TERM_COLS) term_cursor_col = TERM_COLS - 1;
            if (term_cursor_row >= term_line_count) term_line_count = term_cursor_row + 1;
            break;
        case 'J': // Erase in Display
            if (p0 == 0) {
                // Clear from cursor to end of screen
                memset(&term_buf[term_cursor_row][term_cursor_col], ' ',
                       TERM_COLS - term_cursor_col);
                for (int r = term_cursor_row + 1; r < TERM_ROWS; r++) {
                    memset(term_buf[r], ' ', TERM_COLS);
                }
            } else if (p0 == 1) {
                // Clear from start to cursor
                for (int r = 0; r < term_cursor_row; r++) {
                    memset(term_buf[r], ' ', TERM_COLS);
                }
                memset(term_buf[term_cursor_row], ' ', term_cursor_col + 1);
            } else if (p0 == 2 || p0 == 3) {
                // Clear entire screen
                for (int r = 0; r < TERM_ROWS; r++) {
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
                // Clear from cursor to end of line
                memset(&term_buf[term_cursor_row][term_cursor_col], ' ',
                       TERM_COLS - term_cursor_col);
            } else if (p0 == 1) {
                // Clear from start of line to cursor
                memset(term_buf[term_cursor_row], ' ', term_cursor_col + 1);
            } else if (p0 == 2) {
                // Clear entire line
                memset(term_buf[term_cursor_row], ' ', TERM_COLS);
            }
            break;
        case 'G': // Cursor Horizontal Absolute
            term_cursor_col = (p0 > 0) ? p0 - 1 : 0;
            if (term_cursor_col >= TERM_COLS) term_cursor_col = TERM_COLS - 1;
            break;
        case 'd': // Cursor Vertical Absolute
            term_cursor_row = (p0 > 0) ? p0 - 1 : 0;
            if (term_cursor_row >= TERM_ROWS) term_cursor_row = TERM_ROWS - 1;
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
        case 'L': { // Insert Lines
            int n = (p0 > 0) ? p0 : 1;
            for (int j = TERM_ROWS - 1; j >= term_cursor_row + n; j--) {
                memcpy(term_buf[j], term_buf[j - n], TERM_COLS + 1);
            }
            for (int j = term_cursor_row; j < term_cursor_row + n && j < TERM_ROWS; j++) {
                memset(term_buf[j], ' ', TERM_COLS);
                term_buf[j][TERM_COLS] = '\0';
            }
            break;
        }
        case 'M': { // Delete Lines
            int n = (p0 > 0) ? p0 : 1;
            for (int j = term_cursor_row; j + n < TERM_ROWS; j++) {
                memcpy(term_buf[j], term_buf[j + n], TERM_COLS + 1);
            }
            for (int j = TERM_ROWS - n; j < TERM_ROWS; j++) {
                memset(term_buf[j], ' ', TERM_COLS);
                term_buf[j][TERM_COLS] = '\0';
            }
            break;
        }
        // m (SGR - colors/attributes), h/l (mode set/reset), r (scroll region)
        // — ignored silently (no color on e-ink, modes not critical)
        default:
            break;
    }
}

void terminalAppendOutput(const char* data, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];

        // ANSI escape sequence handling
        if (in_escape) {
            if (!in_bracket) {
                if (c == '[') {
                    in_bracket = true;
                    csi_param_count = 0;
                    csi_parsing_num = false;
                    memset(csi_params, 0, sizeof(csi_params));
                    continue;
                }
                if (c == ']') {
                    // OSC sequence — skip until ST (ESC \ or BEL)
                    in_escape = false;
                    in_bracket = false;
                    // Scan ahead for BEL or ESC backslash
                    while (i + 1 < len) {
                        i++;
                        if ((unsigned char)data[i] == 0x07) break; // BEL
                        if ((unsigned char)data[i] == 0x1B && i + 1 < len && data[i+1] == '\\') {
                            i++;
                            break;
                        }
                    }
                    continue;
                }
                // Single char after ESC (e.g. ESC M for reverse index)
                in_escape = false;
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
                    // Empty param (default 0)
                    if (csi_param_count < MAX_CSI_PARAMS) {
                        csi_params[csi_param_count] = 0;
                    }
                    csi_param_count++;
                }
                continue;
            }
            if (c == '?' || c == '>' || c == '!') {
                // Private mode prefix — continue collecting
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

        if (c == '\n') {
            term_cursor_row++;
            term_cursor_col = 0;
            if (term_cursor_row >= TERM_ROWS) {
                terminalScrollUp();
                term_cursor_row = TERM_ROWS - 1;
            }
            if (term_cursor_row >= term_line_count) {
                term_line_count = term_cursor_row + 1;
            }
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
                term_cursor_row++;
                if (term_cursor_row >= TERM_ROWS) {
                    terminalScrollUp();
                    term_cursor_row = TERM_ROWS - 1;
                }
            }
            continue;
        }

        // Printable characters
        if (c >= ' ' && c <= '~') {
            term_buf[term_cursor_row][term_cursor_col] = (char)c;
            term_cursor_col++;
            if (term_cursor_col >= TERM_COLS) {
                term_cursor_col = 0;
                term_cursor_row++;
                if (term_cursor_row >= TERM_ROWS) {
                    terminalScrollUp();
                    term_cursor_row = TERM_ROWS - 1;
                }
                if (term_cursor_row >= term_line_count) {
                    term_line_count = term_cursor_row + 1;
                }
            }
            continue;
        }
        // Non-printable: ignore
    }

    // Auto-scroll to keep cursor visible
    if (term_cursor_row >= term_scroll + ROWS_PER_SCREEN) {
        term_scroll = term_cursor_row - ROWS_PER_SCREEN + 1;
    }
    if (term_cursor_row < term_scroll) {
        term_scroll = term_cursor_row;
    }
}

// --- WiFi ---

void wifiConnect() {
    if (wifi_state == WIFI_CONNECTING || wifi_state == WIFI_CONNECTED) return;
    wifi_state = WIFI_CONNECTING;
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("WiFi: connecting...");
}

void wifiCheck() {
    if (wifi_state == WIFI_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            wifi_state = WIFI_CONNECTED;
            Serial.printf("WiFi: connected, IP=%s\n", WiFi.localIP().toString().c_str());
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

    Serial.printf("SSH: connecting to %s:%d...\n", SSH_HOST, SSH_PORT);

    ssh_sess = ssh_new();
    if (!ssh_sess) {
        Serial.println("SSH: ssh_new failed");
        return false;
    }

    ssh_options_set(ssh_sess, SSH_OPTIONS_HOST, SSH_HOST);
    int port = SSH_PORT;
    ssh_options_set(ssh_sess, SSH_OPTIONS_PORT, &port);
    ssh_options_set(ssh_sess, SSH_OPTIONS_USER, SSH_USER);

    if (ssh_connect(ssh_sess) != SSH_OK) {
        Serial.printf("SSH: connect failed: %s\n", ssh_get_error(ssh_sess));
        ssh_free(ssh_sess);
        ssh_sess = NULL;

        return false;
    }

    if (ssh_userauth_password(ssh_sess, NULL, SSH_PASS) != SSH_AUTH_SUCCESS) {
        Serial.printf("SSH: auth failed: %s\n", ssh_get_error(ssh_sess));
        ssh_disconnect(ssh_sess);
        ssh_free(ssh_sess);
        ssh_sess = NULL;

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
    if (ssh_channel_request_pty_size(ssh_chan, "vt100", TERM_COLS, ROWS_PER_SCREEN) != SSH_OK) {
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

void sshConnectTask(void* param) {
    sshConnect();
    ssh_connecting = false;
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
    char recv_buf[256];
    for (;;) {
        if (!ssh_connected || !ssh_chan) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int nbytes = ssh_channel_read_nonblocking(ssh_chan, recv_buf, sizeof(recv_buf), 0);
        if (nbytes > 0) {
            xSemaphoreTake(state_mutex, portMAX_DELAY);
            terminalAppendOutput(recv_buf, nbytes);
            xSemaphoreGive(state_mutex);
            term_render_requested = true;
        } else if (nbytes == SSH_ERROR || ssh_channel_is_eof(ssh_chan)) {
            Serial.println("SSH: channel closed by remote");
            ssh_connected = false;
            term_render_requested = true;
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// --- Display Rendering (runs on core 0 only, uses snap_ vars) ---

static int partial_count = 0;

void drawLinesRange(int first_line, int last_line) {
    int text_line = 0, col = 0;

    for (int i = 0; i <= snap_len; i++) {
        int sl = text_line - snap_scroll;

        if (i == snap_cursor && sl >= first_line && sl <= last_line) {
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
        if (c == '\n') { text_line++; col = 0; continue; }

        if (sl >= first_line && sl <= last_line && i != snap_cursor) {
            int x = MARGIN_X + col * CHAR_W;
            int y = MARGIN_Y + sl * CHAR_H + 1;
            display.setCursor(x, y);
            display.print(c);
        }

        col++;
        if (col >= COLS_PER_LINE) { text_line++; col = 0; }

        if (text_line - snap_scroll > last_line && i != snap_cursor) break;
    }
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

void drawStatusBar() {
    LayoutInfo info = computeLayoutFrom(snap_buf, snap_len, snap_cursor);
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
    snprintf(status, sizeof(status), "L%d C%d %s%s",
             info.cursor_line + 1, info.cursor_col + 1,
             mods,
             battery_pct >= 0 ? "" : "");
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
    for (int i = 0; i < TERM_ROWS; i++) {
        memcpy(term_snap_buf[i], term_buf[i], TERM_COLS + 1);
    }
    term_snap_lines  = term_line_count;
    term_snap_scroll = term_scroll;
    term_snap_crow   = term_cursor_row;
    term_snap_ccol   = term_cursor_col;
}

void drawTerminalLines(int first_line, int last_line) {
    for (int sl = first_line; sl <= last_line && sl < ROWS_PER_SCREEN; sl++) {
        int buf_row = term_snap_scroll + sl;
        if (buf_row < 0 || buf_row >= TERM_ROWS) continue;

        for (int c = 0; c < TERM_COLS; c++) {
            char ch = term_snap_buf[buf_row][c];
            int x = MARGIN_X + c * CHAR_W;
            int y = MARGIN_Y + sl * CHAR_H;

            // Draw cursor
            if (buf_row == term_snap_crow && c == term_snap_ccol) {
                display.fillRect(x, y, CHAR_W, CHAR_H, GxEPD_BLACK);
                if (ch != ' ') {
                    display.setTextColor(GxEPD_WHITE);
                    display.setCursor(x, y + 1);
                    display.print(ch);
                    display.setTextColor(GxEPD_BLACK);
                }
            } else if (ch != ' ') {
                display.setCursor(x, y + 1);
                display.print(ch);
            }
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
        snprintf(status, sizeof(status), "SSH...");
    } else if (ssh_connected) {
        const char* net = (modem_state == MODEM_PPP_UP) ? "4G" : "WiFi";
        snprintf(status, sizeof(status), "%s %s@%s", net, SSH_USER, SSH_HOST);
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

void refreshLines(int first_line, int last_line) {
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
        drawStatusBar();
    } while (display.nextPage());
}

void refreshAllPartial() {
    partial_count++;

    display.setPartialWindow(0, 0, SCREEN_W, SCREEN_H);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(NULL);
        drawLinesRange(0, ROWS_PER_SCREEN - 1);
        drawStatusBar();
    } while (display.nextPage());
}

void refreshFullClean() {
    partial_count = 0;
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(NULL);
        drawLinesRange(0, ROWS_PER_SCREEN - 1);
        drawStatusBar();
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
    refreshFullClean();
    prev_layout = computeLayoutFrom(snap_buf, snap_len, snap_cursor);

    AppMode last_mode = MODE_NOTEPAD;
    unsigned long term_last_render = 0;

    for (;;) {
        AppMode cur_mode = app_mode;

        // Mode switch — full redraw
        if (cur_mode != last_mode) {
            last_mode = cur_mode;
            partial_count = 0;
            if (cur_mode == MODE_TERMINAL) {
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                snapshotTerminalState();
                xSemaphoreGive(state_mutex);
                renderTerminalFullClean();
            } else if (cur_mode == MODE_COMMAND) {
                // Draw command prompt overlay
                renderCommandPrompt();
            } else {
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                snapshotState();
                xSemaphoreGive(state_mutex);
                refreshFullClean();
                prev_layout = computeLayoutFrom(snap_buf, snap_len, snap_cursor);
            }
            render_requested = false;
            term_render_requested = false;
            continue;
        }

        // --- Terminal mode ---
        if (cur_mode == MODE_TERMINAL) {
            if (term_render_requested) {
                // Debounce: wait 200ms to batch fast output (reduces e-ink ghosting)
                unsigned long now = millis();
                if (now - term_last_render < 200) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                term_render_requested = false;
                term_last_render = now;

                xSemaphoreTake(state_mutex, portMAX_DELAY);
                snapshotTerminalState();
                xSemaphoreGive(state_mutex);

                if (partial_count >= 20) {
                    renderTerminalFullClean();
                } else {
                    renderTerminal();
                }
            }
            vTaskDelay(1);
            continue;
        }

        // --- Command mode ---
        if (cur_mode == MODE_COMMAND) {
            if (render_requested) {
                render_requested = false;
                renderCommandPrompt();
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
        xSemaphoreGive(state_mutex);

        LayoutInfo cur = computeLayoutFrom(snap_buf, snap_len, snap_cursor);

        if (cur.cursor_line < snap_scroll) {
            snap_scroll = cur.cursor_line;
        }
        if (cur.cursor_line >= snap_scroll + ROWS_PER_SCREEN) {
            snap_scroll = cur.cursor_line - ROWS_PER_SCREEN + 1;
        }
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        scroll_line = snap_scroll;
        xSemaphoreGive(state_mutex);

        if (partial_count >= 20) {
            refreshFullClean();
            prev_layout = cur;
            continue;
        }

        int line_delta = abs(cur.cursor_line - prev_layout.cursor_line);
        if (line_delta > ROWS_PER_SCREEN) {
            refreshAllPartial();
            prev_layout = cur;
            continue;
        }

        int old_sl = prev_layout.cursor_line - snap_scroll;
        int new_sl = cur.cursor_line - snap_scroll;

        if (cur.total_lines != prev_layout.total_lines) {
            int from = min(old_sl, new_sl);
            if (from < 0) from = 0;
            refreshLines(from, ROWS_PER_SCREEN - 1);
        } else {
            int min_l = min(old_sl, new_sl);
            int max_l = max(old_sl, new_sl);
            if (min_l < 0) min_l = 0;
            refreshLines(min_l, max_l);
        }

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
    if (IS_SYM(row, col_rev))    { sym_mode = !sym_mode; return true; }
    if (IS_ALT(row, col_rev))    { alt_mode = !alt_mode; return true; }
    if (IS_MIC(row, col_rev))    {
        unsigned long now = millis();
        if (now - mic_last_press < MIC_DOUBLE_TAP_MS) {
            // Double-tap: switch to terminal, auto-connect if needed
            mic_last_press = 0;
            app_mode = MODE_TERMINAL;
            if (!ssh_connected && !ssh_connecting) {
                if (wifi_state != WIFI_CONNECTED && wifi_state != WIFI_CONNECTING) {
                    wifiConnect();
                }
            }
            return false;
        }
        mic_last_press = now;
        return false;  // Wait for potential double-tap
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
            } else return false;
        }
        // Alt+F: Force full e-ink refresh (works in nav mode too)
        else if (alt_mode && base == 'f') { alt_mode = false; partial_count = 100; render_requested = true; return false; }
        else return false;
        return true;
    }

    char c;
    if (sym_mode)        c = keymap_sym[row][col_rev];
    else if (shift_held) c = keymap_upper[row][col_rev];
    else                 c = keymap_lower[row][col_rev];

    if (c == 0) return false;

    if (c == '\b') {
        if (cursor_pos > 0) {
            memmove(&text_buf[cursor_pos - 1], &text_buf[cursor_pos], text_len - cursor_pos);
            text_len--;
            cursor_pos--;
            text_buf[text_len] = '\0';
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

    // Modifier keys
    if (IS_LSHIFT(row, col_rev)) { shift_held = !shift_held; return false; }
    if (IS_RSHIFT(row, col_rev)) { nav_mode = !nav_mode; return true; }
    if (IS_SYM(row, col_rev))    { sym_mode = !sym_mode; return true; }
    if (IS_ALT(row, col_rev))    { alt_mode = !alt_mode; return true; }
    if (IS_MIC(row, col_rev))    {
        unsigned long now = millis();
        if (now - mic_last_press < MIC_DOUBLE_TAP_MS) {
            // Double-tap: switch to notepad
            mic_last_press = 0;
            app_mode = MODE_NOTEPAD;
            return false;
        }
        mic_last_press = now;
        return false;  // Wait for potential double-tap
    }
    if (IS_DEAD(row, col_rev))   { return false; }

    // Nav mode: WASD sends arrow escape sequences
    if (nav_mode) {
        char base = keymap_lower[row][col_rev];
        if (base == 'w') { sshSendString("\x1b[A", 3); return false; }  // Up
        if (base == 's') { sshSendString("\x1b[B", 3); return false; }  // Down
        if (base == 'd') { sshSendString("\x1b[C", 3); return false; }  // Right
        if (base == 'a') { sshSendString("\x1b[D", 3); return false; }  // Left
        if (base == '\b') { sshSendKey(0x7F); return false; }
        return false;
    }

    // Alt = Ctrl modifier
    if (alt_mode) {
        char base = keymap_lower[row][col_rev];
        // Alt+Space = Escape
        if (base == ' ') { sshSendKey(0x1B); alt_mode = false; return true; }
        // Alt+Enter = Tab
        if (base == '\n') { sshSendKey('\t'); alt_mode = false; return true; }
        // Alt+Backspace = DEL
        if (base == '\b') { sshSendKey(0x7F); alt_mode = false; return false; }
        // Alt+letter = Ctrl+letter (a=0x01 .. z=0x1A)
        if (base >= 'a' && base <= 'z') {
            sshSendKey(base - 'a' + 1);
            alt_mode = false;
            return false;
        }
        return false;
    }

    char c;
    if (sym_mode)        c = keymap_sym[row][col_rev];
    else if (shift_held) c = keymap_upper[row][col_rev];
    else                 c = keymap_lower[row][col_rev];

    if (c == 0) return false;

    if (c == '\b') {
        sshSendKey(0x7F);  // Send DEL
        return false;
    }

    if (c == '\n') {
        sshSendKey('\r');
        return false;
    }

    if (c >= ' ' && c <= '~') {
        sshSendKey(c);
        if (shift_held) shift_held = false;
        return false;
    }

    return false;
}

// --- Command Processor ---

void executeCommand(const char* cmd) {
    // Single-char shortcuts: p=paste, d=disconnect, r=refresh, 4=4g, s=status, ?/h=help, q=quit
    if (strcmp(cmd, "p") == 0 || strcmp(cmd, "paste") == 0) {
        if (!ssh_connected || !ssh_chan) {
            snprintf(cmd_result, sizeof(cmd_result), "SSH not connected");
        } else if (text_len == 0) {
            snprintf(cmd_result, sizeof(cmd_result), "Notepad empty");
        } else {
            for (int i = 0; i < text_len; i += 64) {
                int chunk = (text_len - i > 64) ? 64 : (text_len - i);
                ssh_channel_write(ssh_chan, &text_buf[i], chunk);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            snprintf(cmd_result, sizeof(cmd_result), "Pasted %d chars", text_len);
        }
    } else if (strcmp(cmd, "d") == 0 || strcmp(cmd, "dc") == 0) {
        sshDisconnect();
        snprintf(cmd_result, sizeof(cmd_result), "Disconnected");
    } else if (strcmp(cmd, "r") == 0 || strcmp(cmd, "refresh") == 0) {
        partial_count = 100;
        snprintf(cmd_result, sizeof(cmd_result), "Full refresh queued");
    } else if (strcmp(cmd, "4") == 0 || strcmp(cmd, "4g") == 0) {
        if (modem_state == MODEM_OFF || modem_state == MODEM_FAILED) {
            modemStartAsync();
            snprintf(cmd_result, sizeof(cmd_result), "4G starting...");
        } else if (modem_state == MODEM_PPP_UP) {
            modemStop();
            snprintf(cmd_result, sizeof(cmd_result), "4G stopped");
        } else {
            snprintf(cmd_result, sizeof(cmd_result), "4G busy...");
        }
    } else if (strcmp(cmd, "s") == 0 || strcmp(cmd, "status") == 0) {
        const char* ws = "off";
        if (wifi_state == WIFI_CONNECTED) ws = "ok";
        else if (wifi_state == WIFI_CONNECTING) ws = "...";
        const char* ms = "off";
        if (modem_state == MODEM_PPP_UP) ms = "ok";
        else if (modem_state >= MODEM_POWERING_ON && modem_state <= MODEM_REGISTERED) ms = "...";
        else if (modem_state == MODEM_FAILED) ms = "fail";
        snprintf(cmd_result, sizeof(cmd_result), "WiFi:%s 4G:%s SSH:%s Bat:%d%%",
                 ws, ms, ssh_connected ? "ok" : "off", battery_pct);
    } else if (strcmp(cmd, "?") == 0 || strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0) {
        snprintf(cmd_result, sizeof(cmd_result), "(p)aste (d)c (r)efresh (4)g (s)tatus");
    } else if (strlen(cmd) == 0) {
        cmd_result_valid = false;
        return;
    } else {
        snprintf(cmd_result, sizeof(cmd_result), "? (p)aste (d)c (r)ef (4)g (s)tat");
    }
    cmd_result_valid = true;
}

bool handleCommandKeyPress(int event_code) {
    int key_num = (event_code & 0x7F);
    int idx = key_num - 1;
    int row = idx / KEYPAD_COLS;
    int col_raw = idx % KEYPAD_COLS;
    int col_rev = (KEYPAD_COLS - 1) - col_raw;

    if (row < 0 || row >= KEYPAD_ROWS || col_rev < 0 || col_rev >= KEYPAD_COLS) return false;

    if (IS_SHIFT(row, col_rev)) { shift_held = !shift_held; return false; }
    if (IS_SYM(row, col_rev))   { sym_mode = !sym_mode; return true; }
    if (IS_ALT(row, col_rev))   { return false; }
    if (IS_MIC(row, col_rev))   {
        // MIC exits command mode back to previous mode
        app_mode = cmd_return_mode;
        return false;
    }
    if (IS_DEAD(row, col_rev))  { return false; }

    char c;
    if (sym_mode)        c = keymap_sym[row][col_rev];
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
    int prompt_y = SCREEN_H - STATUS_H - CHAR_H * 3;  // 3 lines from bottom
    int region_h = SCREEN_H - prompt_y;

    display.setPartialWindow(0, prompt_y, SCREEN_W, region_h);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setFont(NULL);

        // Show result from last command
        if (cmd_result_valid) {
            display.setCursor(MARGIN_X, prompt_y + 1);
            display.print(cmd_result);
        }

        // Draw prompt line
        int py = prompt_y + CHAR_H + 2;
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
        display.print("[CMD] ? for help | MIC to exit");
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
    // Check WiFi / modem / battery status periodically
    static unsigned long last_net_check = 0;
    if (millis() - last_net_check > 1000) {
        last_net_check = millis();
        wifiCheck();
        updateBattery();
        // Auto-connect SSH once WiFi is ready (if we switched to terminal and need it)
        if (app_mode == MODE_TERMINAL && wifi_state == WIFI_CONNECTED
            && !ssh_connected && !ssh_connecting) {
            sshConnectAsync();
        }
        if (app_mode == MODE_TERMINAL && (wifi_state == WIFI_CONNECTED || wifi_state == WIFI_FAILED
            || modem_state != MODEM_OFF)) {
            term_render_requested = true;  // Update status bar
        }
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
