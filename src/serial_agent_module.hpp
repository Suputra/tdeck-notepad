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

static const char* agentModeName(AppMode mode) {
    switch (mode) {
        case MODE_NOTEPAD:  return "notepad";
        case MODE_TERMINAL: return "terminal";
        case MODE_KEYBOARD: return "keyboard";
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
    } else if (mode == MODE_KEYBOARD) {
        needs_render = handleKeyboardModeKeyPress(event_code);
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
    if (strcasecmp(name, "LSHIFT") == 0 || strcasecmp(name, "SHIFT") == 0) return agentPressKeyLocked(3, 9);
    if (strcasecmp(name, "RSHIFT") == 0 || strcasecmp(name, "NAV") == 0) return agentPressKeyLocked(3, 5);
    if (strcasecmp(name, "SPACE") == 0) return agentPressKeyLocked(3, 7);
    if (strcasecmp(name, "ENTER") == 0 || strcasecmp(name, "RETURN") == 0 || strcasecmp(name, "RET") == 0) return agentPressKeyLocked(2, 9);
    if (strcasecmp(name, "BACKSPACE") == 0 || strcasecmp(name, "BS") == 0 || strcasecmp(name, "BKSP") == 0) return agentPressKeyLocked(1, 9);

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
        if (err) *err = "disable ALT/NAV before TEXT/CHAR";
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
        "AGENT OK STATE mode=%s text_len=%d cursor=%d scroll=%d cmd_len=%d wifi=%d ssh=%d bt=%s heap=%d\n",
        agentModeName(app_mode),
        text_len,
        cursor_pos,
        scroll_line,
        cmd_len,
        (int)wifi_state,
        ssh_connected ? 1 : 0,
        btStatusShort(),
        ESP.getFreeHeap()
    );
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
        agentReplyOk("commands=PING HELP MODE STATE KEY KEYNAME PRESS CHAR TEXT MIC CMD WAIT RENDER BOOTOFF");
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

    xSemaphoreTake(state_mutex, portMAX_DELAY);

    if (strcasecmp(p, "MODE") == 0) {
        agentReplyOk("MODE %s", agentModeName(app_mode));
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "STATE") == 0) {
        agentReportStateLocked();
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

    if (strcasecmp(p, "KEYNAME") == 0) {
        if (!arg || *arg == '\0') {
            agentReplyErr("usage: @KEYNAME <name|single-char>");
            xSemaphoreGive(state_mutex);
            return;
        }
        if (!agentPressNamedKeyLocked(arg)) {
            agentReplyErr("unknown keyname");
            xSemaphoreGive(state_mutex);
            return;
        }
        agentReplyOk("KEYNAME %s", arg);
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "PRESS") == 0 || strcasecmp(p, "TAP") == 0) {
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

    if (strcasecmp(p, "MIC") == 0) {
        if (!arg || *arg == '\0') {
            agentReplyErr("usage: @MIC SINGLE|DOUBLE");
            xSemaphoreGive(state_mutex);
            return;
        }
        if (strcasecmp(arg, "SINGLE") == 0) {
            agentPressKeyLocked(3, 6);
            agentReplyOk("MIC SINGLE");
            xSemaphoreGive(state_mutex);
            return;
        }
        if (strcasecmp(arg, "DOUBLE") == 0) {
            agentPressKeyLocked(3, 6);
            agentPressKeyLocked(3, 6);
            agentReplyOk("MIC DOUBLE");
            xSemaphoreGive(state_mutex);
            return;
        }
        agentReplyErr("usage: @MIC SINGLE|DOUBLE");
        xSemaphoreGive(state_mutex);
        return;
    }

    if (strcasecmp(p, "CHAR") == 0) {
        if (!arg || *arg == '\0') {
            agentReplyErr("usage: @CHAR <c>");
            xSemaphoreGive(state_mutex);
            return;
        }
        char dec[8];
        int n = agentDecodeEscapes(arg, dec, sizeof(dec));
        if (n != 1) {
            agentReplyErr("CHAR expects exactly one character");
            xSemaphoreGive(state_mutex);
            return;
        }
        const char* err = NULL;
        if (!agentTypeOneCharLocked(dec[0], &err)) {
            agentReplyErr("CHAR failed: %s", err ? err : "unknown");
            xSemaphoreGive(state_mutex);
            return;
        }
        agentReplyOk("CHAR");
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
        executeCommand(arg);
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
