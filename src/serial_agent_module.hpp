#pragma once

#ifndef TDECK_AGENT_DEBUG
#define TDECK_AGENT_DEBUG 0
#endif

#if TDECK_AGENT_DEBUG

#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

#define AGENT_RX_BUF_LEN 256

static char agent_rx_buf[AGENT_RX_BUF_LEN];
static int  agent_rx_len = 0;
static constexpr uint32_t AGENT_STATE_LOCK_TIMEOUT_MS = 1000;

static const char* agentModeName(AppMode mode) {
    switch (mode) {
        case MODE_NOTEPAD:  return "notepad";
        case MODE_TERMINAL: return "terminal";
        case MODE_COMMAND:  return "command";
        default:            return "unknown";
    }
}

static void agentReplyV(const char* status, const char* fmt, va_list ap) {
    char msg[192];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    Serial.printf("AGENT %s %s\n", status, msg);
}

static void agentReplyOk(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    agentReplyV("OK", fmt, ap);
    va_end(ap);
}

static void agentReplyErr(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    agentReplyV("ERR", fmt, ap);
    va_end(ap);
}

static bool agentDispatchEventLocked(int event_code) {
    bool needs_render = false;
    AppMode mode = app_mode;
    if (mode == MODE_NOTEPAD) {
        needs_render = handleNotepadKeyPress(event_code);
    } else if (mode == MODE_TERMINAL) {
        needs_render = handleTerminalKeyPress(event_code);
    } else if (mode == MODE_COMMAND) {
        needs_render = handleCommandKeyPress(event_code);
    }

    if (needs_render) {
        AppMode cur = app_mode;
        if (cur == MODE_NOTEPAD || cur == MODE_COMMAND) render_requested = true;
        else term_render_requested = true;
    }
    return true;
}

static bool agentBuildEventCode(int row, int col_rev, int* event_code_out) {
    if (!event_code_out) return false;
    if (row < 0 || row >= KEYPAD_ROWS || col_rev < 0 || col_rev >= KEYPAD_COLS) return false;
    int col_raw = (KEYPAD_COLS - 1) - col_rev;
    int idx = row * KEYPAD_COLS + col_raw;
    int key_num = idx + 1;
    *event_code_out = (0x80 | key_num);
    return true;
}

static bool agentPressKeyLocked(int row, int col_rev) {
    int event_code = 0;
    if (!agentBuildEventCode(row, col_rev, &event_code)) return false;
    return agentDispatchEventLocked(event_code);
}

static bool agentFindInMap(char target, const char map[KEYPAD_ROWS][KEYPAD_COLS], int* out_row, int* out_col_rev) {
    for (int r = 0; r < KEYPAD_ROWS; r++) {
        for (int c = 0; c < KEYPAD_COLS; c++) {
            if (map[r][c] == target) {
                if (out_row) *out_row = r;
                if (out_col_rev) *out_col_rev = c;
                return true;
            }
        }
    }
    return false;
}

static bool agentPressNamedKeyLocked(const char* name) {
    if (!name || name[0] == '\0') return false;

    if (strcasecmp(name, "MIC") == 0) return agentPressKeyLocked(3, 6);
    if (strcasecmp(name, "ALT") == 0) return agentPressKeyLocked(2, 0);
    if (strcasecmp(name, "SYM") == 0) return agentPressKeyLocked(3, 8);
    if (strcasecmp(name, "LSHIFT") == 0) return agentPressKeyLocked(3, 9);
    if (strcasecmp(name, "RSHIFT") == 0) return agentPressKeyLocked(3, 5);
    if (strcasecmp(name, "SPACE") == 0) return agentPressKeyLocked(3, 7);
    if (strcasecmp(name, "ENTER") == 0) return agentPressKeyLocked(2, 9);
    if (strcasecmp(name, "BACKSPACE") == 0) return agentPressKeyLocked(1, 9);

    // Single-character key tokens map to physical matrix positions.
    // This allows arbitrary key tapping by base legend (e.g. q, a, z, 1, ?, -).
    if (name[0] != '\0' && name[1] == '\0') {
        int row = -1;
        int col_rev = -1;
        char c = name[0];
        char lower = (char)tolower((unsigned char)c);
        if (agentFindInMap(lower, keymap_lower, &row, &col_rev)) return agentPressKeyLocked(row, col_rev);
        if (agentFindInMap(c, keymap_sym, &row, &col_rev)) return agentPressKeyLocked(row, col_rev);
    }

    return false;
}

static bool agentTypeOneCharLocked(char c, const char** err) {
    if (c == '\r') c = '\n';
    if (c == '\t') {
        if (err) *err = "TAB requires explicit key combos in this firmware";
        return false;
    }
    if (alt_mode || nav_mode) {
        if (err) *err = "disable ALT/NAV before TEXT";
        return false;
    }

    int row = -1;
    int col_rev = -1;

    // Keep typing deterministic.
    if (sym_mode) sym_mode = false;

    if (agentFindInMap(c, keymap_lower, &row, &col_rev)) {
        if (shift_held) agentPressKeyLocked(3, 9);
        return agentPressKeyLocked(row, col_rev);
    }

    if (agentFindInMap(c, keymap_upper, &row, &col_rev)) {
        if (!shift_held) agentPressKeyLocked(3, 9);
        return agentPressKeyLocked(row, col_rev);
    }

    if (agentFindInMap(c, keymap_sym, &row, &col_rev)) {
        if (shift_held) agentPressKeyLocked(3, 9);
        agentPressKeyLocked(3, 8);  // SYM one-shot
        return agentPressKeyLocked(row, col_rev);
    }

    if (err) *err = "character not representable on keypad";
    return false;
}

static int agentDecodeEscapes(const char* src, char* dst, int dst_cap) {
    if (!src || !dst || dst_cap <= 0) return -1;
    int n = 0;
    for (int i = 0; src[i] != '\0'; i++) {
        char c = src[i];
        if (c == '\\') {
            char next = src[i + 1];
            if (next == '\0') break;
            i++;
            if (next == 'n') c = '\n';
            else if (next == 'r') c = '\r';
            else if (next == 't') c = '\t';
            else if (next == '\\') c = '\\';
            else if (next == 's') c = ' ';
            else c = next;
        }
        if (n >= dst_cap - 1) return -1;
        dst[n++] = c;
    }
    dst[n] = '\0';
    return n;
}

static char* agentTrim(char* s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
    return s;
}

static void agentReportStateLocked() {
    Serial.printf(
        "AGENT OK STATE mode=%s text_len=%d cursor=%d scroll=%d cmd_len=%d wifi=%d ssh=%d bt=%s touch=%d heap=%d up=%d/%d up_run=%d down=%d/%d down_run=%d\n",
        agentModeName(app_mode),
        text_len,
        cursor_pos,
        scroll_line,
        cmd_len,
        (int)wifi_state,
        ssh_connected ? 1 : 0,
        btStatusShort(),
        touch_available ? 1 : 0,
        ESP.getFreeHeap(),
        upload_done_count,
        upload_total_count,
        upload_running ? 1 : 0,
        download_done_count,
        download_total_count,
        download_running ? 1 : 0
    );
}

static bool agentTakeStateLock() {
    if (!state_mutex) return false;
    return xSemaphoreTake(state_mutex, pdMS_TO_TICKS(AGENT_STATE_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void agentRunCommand(char* line) {
    char* p = agentTrim(line);
    if (*p == '\0') {
        agentReplyErr("empty command");
        return;
    }

    char* sp = strchr(p, ' ');
    char* arg = NULL;
    if (sp) {
        *sp = '\0';
        arg = agentTrim(sp + 1);
    }

    if (strcasecmp(p, "PING") == 0) {
        agentReplyOk("PONG");
        return;
    }
    if (strcasecmp(p, "HELP") == 0) {
        agentReplyOk("commands=PING HELP STATE RESULT TRACE TERMDBG TERMSNAP TERMHEX TERMRANGE KEY PRESS TEXT CMD WAIT RENDER BOOTOFF");
        return;
    }

    if (strcasecmp(p, "TOUCH") == 0) {
        // Try reading touch data at configured address
        uint8_t addr = touch_i2c_addr;
        Wire.beginTransmission(addr);
        uint8_t ack = Wire.endTransmission();
        if (ack != 0) {
            agentReplyOk("TOUCH addr=0x%02X ack_err=%d avail=%d",
                addr, ack, touch_available ? 1 : 0);
            return;
        }
        Wire.beginTransmission(addr);
        Wire.write((uint8_t)0xD0);
        Wire.write((uint8_t)0x00);
        uint8_t werr = Wire.endTransmission();
        uint8_t raw[7] = {0};
        int avail = 0;
        if (werr == 0) {
            Wire.requestFrom(addr, (uint8_t)7);
            avail = Wire.available();
            for (int i = 0; i < 7 && Wire.available(); i++) raw[i] = Wire.read();
        }
        agentReplyOk("TOUCH addr=0x%02X werr=%d avail=%d raw=%02X %02X %02X %02X %02X %02X %02X",
            addr, werr, avail, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6]);
        return;
    }

    if (strcasecmp(p, "I2CSCAN") == 0) {
        Serial.print("AGENT OK I2C:");
        for (uint8_t addr = 1; addr < 127; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                Serial.printf(" 0x%02X", addr);
            }
        }
        Serial.println();
        return;
    }

    if (strcasecmp(p, "TRACE") == 0) {
        if (!arg || *arg == '\0') {
            agentReplyOk("TRACE enabled=%d", terminalDebugTraceEnabled() ? 1 : 0);
            return;
        }
        if (strcasecmp(arg, "ON") == 0) {
            terminalDebugTraceClear();
            terminalDebugTraceSet(true);
            agentReplyOk("TRACE ON");
            return;
        }
        if (strcasecmp(arg, "OFF") == 0) {
            terminalDebugTraceSet(false);
            agentReplyOk("TRACE OFF");
            return;
        }
        if (strcasecmp(arg, "CLEAR") == 0) {
            terminalDebugTraceClear();
            agentReplyOk("TRACE CLEAR");
            return;
        }
        if (strncasecmp(arg, "DUMP", 4) == 0) {
            int max_bytes = 512;
            char* rest = agentTrim(arg + 4);
            if (rest && *rest != '\0') {
                char* end = NULL;
                long parsed = strtol(rest, &end, 10);
                end = agentTrim(end);
                if (end == rest || (end && *end != '\0') || parsed <= 0) {
                    agentReplyErr("usage: @TRACE DUMP [max_bytes]");
                    return;
                }
                if (parsed > 4096) parsed = 4096;
                max_bytes = (int)parsed;
            }
            terminalDebugTraceDump(max_bytes);
            return;
        }
        agentReplyErr("usage: @TRACE [ON|OFF|CLEAR|DUMP [max_bytes]]");
        return;
    }

    if (strcasecmp(p, "TERMDBG") == 0) {
        terminalDebugStateDump();
        return;
    }

    if (strcasecmp(p, "WAIT") == 0) {
        if (!arg || *arg == '\0') {
            agentReplyErr("usage: @WAIT <ms>");
            return;
        }
        char* end = NULL;
        long ms = strtol(arg, &end, 10);
        end = agentTrim(end);
        if (end == arg || (end && *end != '\0')) {
            agentReplyErr("usage: @WAIT <ms>");
            return;
        }
        if (ms < 0 || ms > 60000) {
            agentReplyErr("wait range 0..60000");
            return;
        }
        delay((uint32_t)ms);
        agentReplyOk("WAIT %ld", ms);
        return;
    }

    if (!agentTakeStateLock()) {
        agentReplyErr("busy: state lock timeout");
        return;
    }

    if (strcasecmp(p, "STATE") == 0) {
        agentReportStateLocked();
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "TERMSNAP") == 0) {
        int row_offset = 0;
        if (arg && *arg != '\0') {
            char* end = NULL;
            long parsed = strtol(arg, &end, 10);
            end = agentTrim(end);
            if (end == arg || (end && *end != '\0') || parsed < 0) {
                agentReplyErr("usage: @TERMSNAP [row]");
                xSemaphoreGive(state_mutex);
                return;
            }
            if (parsed >= ROWS_PER_SCREEN) parsed = ROWS_PER_SCREEN - 1;
            row_offset = (int)parsed;
        }

        int row = term_scroll + row_offset;
        if (row < 0) row = 0;
        if (row >= TERM_ROWS) row = TERM_ROWS - 1;

        char line[TERM_COLS + 1];
        memcpy(line, term_buf[row], TERM_COLS);
        line[TERM_COLS] = '\0';
        int end = TERM_COLS;
        while (end > 0 && line[end - 1] == ' ') end--;
        line[end] = '\0';

        agentReplyOk("TERMSNAP row=%d %s", row_offset, line);
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "TERMHEX") == 0) {
        int row_offset = 0;
        if (arg && *arg != '\0') {
            char* end = NULL;
            long parsed = strtol(arg, &end, 10);
            end = agentTrim(end);
            if (end == arg || (end && *end != '\0') || parsed < 0) {
                agentReplyErr("usage: @TERMHEX [row]");
                xSemaphoreGive(state_mutex);
                return;
            }
            if (parsed >= ROWS_PER_SCREEN) parsed = ROWS_PER_SCREEN - 1;
            row_offset = (int)parsed;
        }
        int row = term_scroll + row_offset;
        if (row < 0) row = 0;
        if (row >= TERM_ROWS) row = TERM_ROWS - 1;

        char hex[TERM_COLS * 3 + 1];
        int pos = 0;
        for (int i = 0; i < TERM_COLS; i++) {
            pos += snprintf(&hex[pos], sizeof(hex) - pos, "%02X", (unsigned char)term_buf[row][i]);
            if (i + 1 < TERM_COLS && pos < (int)sizeof(hex) - 1) hex[pos++] = ' ';
            if (pos >= (int)sizeof(hex) - 1) break;
        }
        hex[pos] = '\0';
        agentReplyOk("TERMHEX row=%d %s", row_offset, hex);
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "TERMRANGE") == 0) {
        int rows = 8;
        if (arg && *arg != '\0') {
            char* end = NULL;
            long parsed = strtol(arg, &end, 10);
            end = agentTrim(end);
            if (end == arg || (end && *end != '\0') || parsed <= 0) {
                agentReplyErr("usage: @TERMRANGE [rows]");
                xSemaphoreGive(state_mutex);
                return;
            }
            if (parsed > ROWS_PER_SCREEN) parsed = ROWS_PER_SCREEN;
            rows = (int)parsed;
        }
        int first = term_scroll;
        if (first < 0) first = 0;
        agentReplyOk("TERMRANGE rows=%d start=%d", rows, first);
        for (int i = 0; i < rows; i++) {
            int row = first + i;
            if (row < 0 || row >= TERM_ROWS) break;
            char line[TERM_COLS + 1];
            memcpy(line, term_buf[row], TERM_COLS);
            line[TERM_COLS] = '\0';
            int end = TERM_COLS;
            while (end > 0 && line[end - 1] == ' ') end--;
            line[end] = '\0';
            Serial.printf("TERMRANGE %02d %s\n", i, line);
        }
        Serial.println("TERMRANGE END");
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "RESULT") == 0) {
        if (!cmd_result_valid || cmd_result_count <= 0 || cmd_result[0][0] == '\0') {
            agentReplyOk("RESULT (empty)");
        } else {
            agentReplyOk("RESULT %s", cmd_result[0]);
        }
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "RENDER") == 0) {
        render_requested = true;
        term_render_requested = true;
        agentReplyOk("RENDER queued");
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "BOOTOFF") == 0) {
        poweroff_requested = true;
        agentReplyOk("BOOTOFF");
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "KEY") == 0) {
        if (!arg || *arg == '\0') {
            agentReplyErr("usage: @KEY <row> <col_rev>");
            xSemaphoreGive(state_mutex);
            return;
        }
        char* row_s = agentTrim(arg);
        char* end1 = NULL;
        long row = strtol(row_s, &end1, 10);
        if (!end1 || end1 == row_s) {
            agentReplyErr("usage: @KEY <row> <col_rev>");
            xSemaphoreGive(state_mutex);
            return;
        }
        char* col_s = agentTrim(end1);
        char* end2 = NULL;
        long col_rev = strtol(col_s, &end2, 10);
        end2 = agentTrim(end2);
        if (!end2 || end2 == col_s || *end2 != '\0') {
            agentReplyErr("usage: @KEY <row> <col_rev>");
            xSemaphoreGive(state_mutex);
            return;
        }
        if (row < 0 || row >= KEYPAD_ROWS || col_rev < 0 || col_rev >= KEYPAD_COLS) {
            agentReplyErr("key out of range");
            xSemaphoreGive(state_mutex);
            return;
        }
        agentPressKeyLocked((int)row, (int)col_rev);
        agentReplyOk("KEY %ld %ld", row, col_rev);
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "PRESS") == 0) {
        if (!arg || *arg == '\0') {
            agentReplyErr("usage: @PRESS <token> [count]");
            xSemaphoreGive(state_mutex);
            return;
        }

        char token[24];
        token[0] = '\0';
        long count = 1;

        int ti = 0;
        while (arg[ti] && !isspace((unsigned char)arg[ti]) && ti < (int)sizeof(token) - 1) {
            token[ti] = arg[ti];
            ti++;
        }
        token[ti] = '\0';

        char* rest = agentTrim(arg + ti);
        if (rest && *rest) {
            char* end = NULL;
            count = strtol(rest, &end, 10);
            end = agentTrim(end);
            if (end == rest || (end && *end != '\0')) {
                agentReplyErr("usage: @PRESS <token> [count]");
                xSemaphoreGive(state_mutex);
                return;
            }
        }

        if (token[0] == '\0' || count < 1 || count > 20) {
            agentReplyErr("PRESS token/count invalid");
            xSemaphoreGive(state_mutex);
            return;
        }

        for (long i = 0; i < count; i++) {
            if (!agentPressNamedKeyLocked(token)) {
                agentReplyErr("PRESS unknown token: %s", token);
                xSemaphoreGive(state_mutex);
                return;
            }
        }
        agentReplyOk("PRESS %s x%ld", token, count);
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "TEXT") == 0) {
        if (!arg) arg = (char*)"";
        char decoded[192];
        int n = agentDecodeEscapes(arg, decoded, sizeof(decoded));
        if (n < 0) {
            agentReplyErr("TEXT too long");
            xSemaphoreGive(state_mutex);
            return;
        }
        int typed = 0;
        for (int i = 0; i < n; i++) {
            const char* err = NULL;
            if (!agentTypeOneCharLocked(decoded[i], &err)) {
                agentReplyErr("TEXT failed at %d: %s", i, err ? err : "unknown");
                xSemaphoreGive(state_mutex);
                return;
            }
            typed++;
        }
        agentReplyOk("TEXT %d", typed);
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "CMD") == 0) {
        if (!arg || *arg == '\0') {
            agentReplyErr("usage: @CMD <command>");
            xSemaphoreGive(state_mutex);
            return;
        }
        cmd_return_mode = app_mode;
        xSemaphoreGive(state_mutex);
        executeCommand(arg);
        if (!agentTakeStateLock()) {
            agentReplyErr("busy: state lock timeout");
            return;
        }
        if (app_mode == MODE_NOTEPAD || app_mode == MODE_COMMAND) render_requested = true;
        else term_render_requested = true;
        agentReplyOk("CMD %s", arg);
        xSemaphoreGive(state_mutex);
        return;
    }

    agentReplyErr("unknown command: %s", p);
    xSemaphoreGive(state_mutex);
}

void agentPollSerial() {
    while (Serial.available() > 0) {
        int b = Serial.read();
        if (b < 0) break;
        char c = (char)b;
        if (c == '\r') continue;
        if (c == '\n') {
            agent_rx_buf[agent_rx_len] = '\0';
            char* line = agentTrim(agent_rx_buf);
            if (line[0] == '@') {
                line++;
                line = agentTrim(line);
                agentRunCommand(line);
            }
            agent_rx_len = 0;
            continue;
        }
        if (agent_rx_len >= AGENT_RX_BUF_LEN - 1) {
            agent_rx_len = 0;
            agentReplyErr("line too long");
            continue;
        }
        agent_rx_buf[agent_rx_len++] = c;
    }
}

#else

inline void agentPollSerial() {}

#endif
