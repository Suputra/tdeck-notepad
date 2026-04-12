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

// MIC tap timestamp (single tap opens command prompt after a short delay)
static unsigned long mic_last_press = 0;
#define MIC_CMD_TAP_DELAY_MS 350

static void buildStatusRight(char* out, size_t out_len, bool include_battery) {
    if (!out || out_len == 0) return;
    char perf[40];
    buildPerfStatusCompact(perf, sizeof(perf));
    if (include_battery && battery_pct >= 0) {
        snprintf(out, out_len, "%s %d%%", perf, battery_pct);
    } else {
        snprintf(out, out_len, "%s", perf);
    }
}

static void drawStatusBarLine(const char* left, const char* right, int bar_y) {
    constexpr int total_cols = SCREEN_W / CHAR_W;
    int right_cols = (right && right[0]) ? (int)strlen(right) : 0;
    if (right_cols > total_cols - 1) right_cols = total_cols - 1;
    int left_max_cols = total_cols - right_cols - (right_cols > 0 ? 1 : 0);
    if (left_max_cols < 0) left_max_cols = 0;

    char left_buf[80];
    left_buf[0] = '\0';
    if (left && left_max_cols > 0) {
        int left_len = (int)strlen(left);
        if (left_len <= left_max_cols) {
            int copy_len = left_len;
            if (copy_len > (int)sizeof(left_buf) - 1) copy_len = (int)sizeof(left_buf) - 1;
            memcpy(left_buf, left, copy_len);
            left_buf[copy_len] = '\0';
        } else if (left_max_cols >= 2) {
            int copy_len = left_max_cols - 1;
            if (copy_len > (int)sizeof(left_buf) - 2) copy_len = (int)sizeof(left_buf) - 2;
            memcpy(left_buf, left, copy_len);
            left_buf[copy_len] = '~';
            left_buf[copy_len + 1] = '\0';
        } else {
            left_buf[0] = '~';
            left_buf[1] = '\0';
        }
    }

    display.setCursor(2, bar_y + 1);
    if (left_buf[0] != '\0') {
        display.print(left_buf);
    }

    if (right_cols > 0 && right) {
        char right_buf[48];
        int copy_len = right_cols;
        if (copy_len > (int)sizeof(right_buf) - 1) copy_len = (int)sizeof(right_buf) - 1;
        memcpy(right_buf, right, copy_len);
        right_buf[copy_len] = '\0';
        int rx = SCREEN_W - copy_len * CHAR_W - 2;
        if (rx < 2) rx = 2;
        display.setCursor(rx, bar_y + 1);
        display.print(right_buf);
    }
}

void drawStatusBar(const LayoutInfo& info) {
    int bar_y = SCREEN_H - STATUS_H;
    display.fillRect(0, bar_y, SCREEN_W, STATUS_H, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(NULL);
    char status[72];
    char mods[16] = "";
    if (snap_shift) strcat(mods, "SH ");
    if (snap_sym)   strcat(mods, "SY ");
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
    char right[48];
    buildStatusRight(right, sizeof(right), true);
    drawStatusBarLine(status, right, bar_y);
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

    char status[72];
    const char* bt_suffix = "";
    if (btIsConnected()) bt_suffix = " +BT";
    else if (btIsEnabled()) bt_suffix = " bt";
    // Build compact connection string
    if (ssh_connecting) {
        snprintf(status, sizeof(status), (vpnActive() ? "VPN SSH...%s" : "SSH...%s"), bt_suffix);
    } else if (ssh_connected) {
        const char* net = vpnActive() ? "VPN" : "WiFi";
        const char* host = ssh_last_host[0] ? ssh_last_host : config_ssh_host;
        snprintf(status, sizeof(status), "%s %s@%s%s", net, config_ssh_user, host, bt_suffix);
    } else if (wifi_state == WIFI_CONNECTED) {
        snprintf(status, sizeof(status), "WiFi %s%s", WiFi.localIP().toString().c_str(), bt_suffix);
    } else if (wifi_state == WIFI_CONNECTING) {
        snprintf(status, sizeof(status), "WiFi...%s", bt_suffix);
    } else if (modemGetState() == MODEM_STATE_SCANNING) {
        snprintf(status, sizeof(status), "4G scan...%s", bt_suffix);
    } else if (modemGetState() == MODEM_STATE_BOOTING) {
        snprintf(status, sizeof(status), "4G boot...%s", bt_suffix);
    } else if (modemGetState() == MODEM_STATE_ON) {
        int csq = modemLastCsq();
        if (csq >= 0 && csq <= 31) snprintf(status, sizeof(status), "4G CSQ %d%s", csq, bt_suffix);
        else snprintf(status, sizeof(status), "4G on%s", bt_suffix);
    } else if (modemGetState() == MODEM_STATE_ERROR) {
        snprintf(status, sizeof(status), "4G err%s", bt_suffix);
    } else {
        snprintf(status, sizeof(status), btIsConnected() ? "BT %s" : "No net%s",
                 btIsConnected() ? btPeerAddress() : bt_suffix);
    }
    char right[48];
    buildStatusRight(right, sizeof(right), true);
    drawStatusBarLine(status, right, bar_y);
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

void drawBtTrackpadStatusBar() {
    int bar_y = SCREEN_H - STATUS_H;
    display.fillRect(0, bar_y, SCREEN_W, STATUS_H, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setFont(NULL);
    char status[72];
    if (btIsConnected()) {
        snprintf(status, sizeof(status), "BT trackpad %s", btPeerAddress());
    } else if (btIsEnabled()) {
        snprintf(status, sizeof(status), "BT trackpad waiting...");
    } else {
        snprintf(status, sizeof(status), "BT off");
    }
    char right[48];
    buildStatusRight(right, sizeof(right), true);
    drawStatusBarLine(status, right, bar_y);
}

void renderBtTrackpad() {
    partial_count++;
    display.setPartialWindow(0, 0, SCREEN_W, SCREEN_H);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        drawBtTrackpadStatusBar();
    } while (display.nextPage());
}

void renderBtTrackpadFullClean() {
    partial_count = 0;
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        drawBtTrackpadStatusBar();
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
    uint32_t render_started = millis();
    refreshFullClean(prev_layout);
    perfRecordRenderMs(millis() - render_started);
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
                uint32_t render_started = millis();
                renderTerminalFullClean();
                perfRecordRenderMs(millis() - render_started);
            } else if (cur_mode == MODE_BT) {
                uint32_t render_started = millis();
                renderBtTrackpadFullClean();
                perfRecordRenderMs(millis() - render_started);
            } else if (cur_mode == MODE_COMMAND) {
                uint32_t render_started = millis();
                renderCommandPrompt();
                perfRecordRenderMs(millis() - render_started);
            } else {
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                snapshotState();
                xSemaphoreGive(state_mutex);
                prev_layout = computeLayoutFrom(snap_buf, snap_len, snap_cursor);
                uint32_t render_started = millis();
                refreshFullClean(prev_layout);
                perfRecordRenderMs(millis() - render_started);
            }
            display_idle = true;
            render_requested = false;
            term_render_requested = false;
            continue;
        }

        // --- Terminal mode ---
        if (cur_mode == MODE_TERMINAL) {
            if (term_render_requested || render_requested) {
                term_render_requested = false;
                render_requested = false;

                display_idle = false;
                uint32_t render_started = millis();
                if (cur_mode == MODE_TERMINAL && connect_status_count > 0) {
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
                perfRecordRenderMs(millis() - render_started);
                display_idle = true;
            }
            vTaskDelay(1);
            continue;
        }

        // --- Bluetooth trackpad mode ---
        if (cur_mode == MODE_BT) {
            if (render_requested || term_render_requested) {
                render_requested = false;
                term_render_requested = false;
                display_idle = false;
                uint32_t render_started = millis();
                if (partial_count >= 20) renderBtTrackpadFullClean();
                else renderBtTrackpad();
                perfRecordRenderMs(millis() - render_started);
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
                uint32_t render_started = millis();
                renderCommandPrompt();
                perfRecordRenderMs(millis() - render_started);
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
            uint32_t render_started = millis();
            refreshFullClean(cur);
            perfRecordRenderMs(millis() - render_started);
            display_idle = true;
            prev_layout = cur;
            continue;
        }

        int line_delta = abs(cur.cursor_line - prev_layout.cursor_line);
        if (line_delta > ROWS_PER_SCREEN) {
            uint32_t render_started = millis();
            refreshAllPartial(cur);
            perfRecordRenderMs(millis() - render_started);
            display_idle = true;
            prev_layout = cur;
            continue;
        }

        int old_sl = prev_layout.cursor_line - snap_scroll;
        int new_sl = cur.cursor_line - snap_scroll;

        if (cur.total_lines != prev_layout.total_lines) {
            int from = min(old_sl, new_sl);
            if (from < 0) from = 0;
            uint32_t render_started = millis();
            refreshLines(from, ROWS_PER_SCREEN - 1, cur);
            perfRecordRenderMs(millis() - render_started);
        } else {
            int min_l = min(old_sl, new_sl);
            int max_l = max(old_sl, new_sl);
            if (min_l < 0) min_l = 0;
            uint32_t render_started = millis();
            refreshLines(min_l, max_l, cur);
            perfRecordRenderMs(millis() - render_started);
        }
        display_idle = true;

        prev_layout = cur;
    }
}
