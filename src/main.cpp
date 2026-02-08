#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <GxEPD2_BW.h>
#include <Adafruit_TCA8418.h>
#include <Fonts/FreeMono9pt7b.h>
#include <WiFi.h>
#include <libssh/libssh.h>
#include <esp_task_wdt.h>

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
#define BOARD_6609_EN       41
#define BOARD_A7682E_PWRKEY 40

// --- Display ---

GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT> display(
    GxEPD2_310_GDEQ031T10(BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST, BOARD_EPD_BUSY)
);

// --- Keyboard ---

Adafruit_TCA8418 keypad;

#define KEYPAD_ROWS 4
#define KEYPAD_COLS 10

#define IS_SHIFT(r, c)  ((r) == 3 && ((c) == 5 || (c) == 9))
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

enum AppMode { MODE_NOTEPAD, MODE_TERMINAL };
static volatile AppMode app_mode = MODE_NOTEPAD;

// --- Editor State (shared between cores, protected by mutex) ---

#define MAX_TEXT_LEN    4096
#define CHAR_W          11
#define CHAR_H          15
#define MARGIN_X        4
#define MARGIN_Y        4
#define SCREEN_W        240
#define SCREEN_H        320
#define STATUS_H        14
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
static bool alt_mode    = false;

// Display task snapshot — private to core 0
static char snap_buf[MAX_TEXT_LEN + 1];
static int  snap_len       = 0;
static int  snap_cursor    = 0;
static int  snap_scroll    = 0;
static bool snap_shift     = false;
static bool snap_sym       = false;
static bool snap_alt       = false;

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

// --- SSH State ---

static ssh_session  ssh_sess    = NULL;
static ssh_channel  ssh_chan     = NULL;
static volatile bool ssh_connected = false;
static TaskHandle_t ssh_recv_task_handle = NULL;

// --- WiFi State ---

enum WifiState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED };
static volatile WifiState wifi_state = WIFI_IDLE;

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

void terminalAppendOutput(const char* data, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];

        // ANSI escape sequence handling
        if (in_escape) {
            if (!in_bracket) {
                if (c == '[') { in_bracket = true; continue; }
                // Single char after ESC (e.g. ESC M) — just skip
                in_escape = false;
                continue;
            }
            // Inside ESC[ ... — wait for final letter
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '@' || c == '`') {
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

bool sshConnect() {
    if (wifi_state != WIFI_CONNECTED) {
        Serial.println("SSH: no WiFi");
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
                display.setCursor(x, y + CHAR_H - 3);
                display.print(snap_buf[i]);
                display.setTextColor(GxEPD_BLACK);
            }
        }

        if (i >= snap_len) break;
        char c = snap_buf[i];
        if (c == '\n') { text_line++; col = 0; continue; }

        if (sl >= first_line && sl <= last_line && i != snap_cursor) {
            int x = MARGIN_X + col * CHAR_W;
            int y = MARGIN_Y + sl * CHAR_H + CHAR_H - 3;
            display.setCursor(x, y);
            display.print(c);
        }

        col++;
        if (col >= COLS_PER_LINE) { text_line++; col = 0; }

        if (text_line - snap_scroll > last_line && i != snap_cursor) break;
    }
}

void drawStatusBar() {
    LayoutInfo info = computeLayoutFrom(snap_buf, snap_len, snap_cursor);
    int bar_y = SCREEN_H - STATUS_H;
    display.fillRect(0, bar_y, SCREEN_W, STATUS_H, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(&FreeMono9pt7b);
    display.setCursor(2, SCREEN_H - 3);
    char status[50];
    snprintf(status, sizeof(status), "L%d C%d %dch %s%s%s",
             info.cursor_line + 1, info.cursor_col + 1, snap_len,
             snap_shift ? "[SH]" : "",
             snap_sym ? "[SYM]" : "",
             snap_alt ? "[NAV]" : "");
    display.print(status);
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
                    display.setCursor(x, y + CHAR_H - 3);
                    display.print(ch);
                    display.setTextColor(GxEPD_BLACK);
                }
            } else if (ch != ' ') {
                display.setCursor(x, y + CHAR_H - 3);
                display.print(ch);
            }
        }
    }
}

void drawTerminalStatusBar() {
    int bar_y = SCREEN_H - STATUS_H;
    display.fillRect(0, bar_y, SCREEN_W, STATUS_H, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(&FreeMono9pt7b);
    display.setCursor(2, SCREEN_H - 3);

    char status[50];
    if (ssh_connected) {
        snprintf(status, sizeof(status), "[TERM] %s@%s", SSH_USER, SSH_HOST);
    } else if (wifi_state == WIFI_CONNECTED) {
        snprintf(status, sizeof(status), "[TERM] %s", WiFi.localIP().toString().c_str());
    } else if (wifi_state == WIFI_CONNECTING) {
        snprintf(status, sizeof(status), "[TERM] WiFi...");
    } else {
        snprintf(status, sizeof(status), "[TERM] No WiFi");
    }
    display.print(status);
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
        display.setFont(&FreeMono9pt7b);
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
        display.setFont(&FreeMono9pt7b);
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
        display.setFont(&FreeMono9pt7b);
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
        display.setFont(&FreeMono9pt7b);
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
        display.setFont(&FreeMono9pt7b);
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
                // Debounce: wait 100ms to batch fast output
                unsigned long now = millis();
                if (now - term_last_render < 100) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                term_render_requested = false;
                term_last_render = now;

                xSemaphoreTake(state_mutex, portMAX_DELAY);
                snapshotTerminalState();
                xSemaphoreGive(state_mutex);

                if (partial_count >= 30) {
                    renderTerminalFullClean();
                } else {
                    renderTerminal();
                }
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

        if (partial_count >= 30) {
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

    if (IS_SHIFT(row, col_rev)) { shift_held = !shift_held; return false; }
    if (IS_SYM(row, col_rev))   { sym_mode = !sym_mode; return true; }
    if (IS_ALT(row, col_rev))   { alt_mode = !alt_mode; return true; }
    if (IS_MIC(row, col_rev))   {
        app_mode = MODE_TERMINAL;
        // Auto-connect WiFi when entering terminal mode
        if (wifi_state == WIFI_IDLE || wifi_state == WIFI_FAILED) {
            wifiConnect();
        }
        return false;  // mode switch handled by display task
    }
    if (IS_DEAD(row, col_rev))  { return false; }

    if (alt_mode) {
        char base = keymap_lower[row][col_rev];
        if (base == 'r')      cursorUp();
        else if (base == 'd') cursorLeft();
        else if (base == 'c') cursorDown();
        else if (base == 'g') cursorRight();
        else if (base == '\b') {
            if (cursor_pos > 0) {
                memmove(&text_buf[cursor_pos - 1], &text_buf[cursor_pos], text_len - cursor_pos);
                text_len--;
                cursor_pos--;
                text_buf[text_len] = '\0';
            } else return false;
        }
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

    if (IS_SHIFT(row, col_rev)) { shift_held = !shift_held; return false; }
    if (IS_SYM(row, col_rev))   { sym_mode = !sym_mode; return true; }
    if (IS_ALT(row, col_rev))   { alt_mode = !alt_mode; return true; }
    if (IS_MIC(row, col_rev))   {
        app_mode = MODE_NOTEPAD;
        return false;
    }
    if (IS_DEAD(row, col_rev))  { return false; }

    if (alt_mode) {
        char base = keymap_lower[row][col_rev];
        // Arrow keys
        if (base == 'r') { sshSendString("\x1b[A", 3); return false; }  // Up
        if (base == 'c') { sshSendString("\x1b[B", 3); return false; }  // Down
        if (base == 'g') { sshSendString("\x1b[C", 3); return false; }  // Right
        if (base == 'd') { sshSendString("\x1b[D", 3); return false; }  // Left
        // Backspace in nav mode
        if (base == '\b') { sshSendKey(0x7F); return false; }
        // Alt+S: SSH connect
        if (base == 's') { sshConnectAsync(); return true; }
        // Alt+Q: SSH disconnect
        if (base == 'q') { sshDisconnect(); return true; }
        // Alt+W: WiFi reconnect
        if (base == 'w') { wifi_state = WIFI_IDLE; wifiConnect(); return true; }
        return false;
    }

    char c;
    if (sym_mode)        c = keymap_sym[row][col_rev];
    else if (shift_held) c = keymap_upper[row][col_rev];
    else                 c = keymap_lower[row][col_rev];

    if (c == 0) return false;

    if (c == '\b') {
        sshSendKey(0x7F);  // Send DEL
        return false;  // no local echo
    }

    if (c == '\n') {
        sshSendKey('\r');
        return false;
    }

    if (c >= ' ' && c <= '~') {
        sshSendKey(c);
        if (shift_held) shift_held = false;
        return false;  // no local echo
    }

    return false;
}

// --- Setup & Loop ---

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("T-Deck Pro Notepad + Terminal starting...");

    // Disable task watchdog — SSH blocking calls would trigger it
    esp_task_wdt_deinit();

    // Disable unused peripherals
    pinMode(BOARD_LORA_EN, OUTPUT);       digitalWrite(BOARD_LORA_EN, LOW);
    pinMode(BOARD_GPS_EN, OUTPUT);        digitalWrite(BOARD_GPS_EN, LOW);
    pinMode(BOARD_1V8_EN, OUTPUT);        digitalWrite(BOARD_1V8_EN, LOW);
    pinMode(BOARD_6609_EN, OUTPUT);       digitalWrite(BOARD_6609_EN, LOW);
    pinMode(BOARD_A7682E_PWRKEY, OUTPUT); digitalWrite(BOARD_A7682E_PWRKEY, LOW);

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

    Serial.println("Ready. Start typing! Press MIC for terminal mode.");
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
}

// Core 1: keyboard polling — never blocks on display
void loop() {
    // Check WiFi status periodically
    static unsigned long last_wifi_check = 0;
    if (millis() - last_wifi_check > 1000) {
        last_wifi_check = millis();
        wifiCheck();
        if (app_mode == MODE_TERMINAL && (wifi_state == WIFI_CONNECTED || wifi_state == WIFI_FAILED)) {
            term_render_requested = true;  // Update status bar
        }
    }

    while (keypad.available() > 0) {
        int ev = keypad.getEvent();
        if (!(ev & 0x80)) continue;  // skip release events

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        bool needs_render = false;
        if (app_mode == MODE_NOTEPAD) {
            needs_render = handleNotepadKeyPress(ev);
        } else {
            needs_render = handleTerminalKeyPress(ev);
        }
        xSemaphoreGive(state_mutex);

        if (needs_render) {
            if (app_mode == MODE_NOTEPAD) {
                render_requested = true;
            } else {
                term_render_requested = true;
            }
        }
    }
}
