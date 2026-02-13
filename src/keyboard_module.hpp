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
