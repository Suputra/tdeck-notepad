// --- WiFi ---

struct WiFiAttemptResult {
    bool connected;
    wl_status_t status;
    bool timed_out;
};

static constexpr const char* WIFI_NTP_SERVER_PRIMARY = "pool.ntp.org";
static constexpr const char* WIFI_NTP_SERVER_SECONDARY = "time.google.com";
static constexpr uint8_t WIFI_NTP_SYNC_TRIES = 10;
static constexpr uint32_t WIFI_NTP_SYNC_RETRY_MS = 250;

bool wifiSyncClockNtp() {
    if (WiFi.status() != WL_CONNECTED) return false;

    configTime(0, 0, WIFI_NTP_SERVER_PRIMARY, WIFI_NTP_SERVER_SECONDARY);
    // configTime() can reset TZ to UTC; reapply configured POSIX TZ so local dates stay correct.
    timeSyncSetTimeZone(timeSyncGetTimeZone());
    struct tm tm = {};
    for (uint8_t i = 0; i < WIFI_NTP_SYNC_TRIES; i++) {
        if (getLocalTime(&tm, WIFI_NTP_SYNC_RETRY_MS)) {
            timeSyncMarkNtp();
            return true;
        }
        delay(WIFI_NTP_SYNC_RETRY_MS);
    }
    return false;
}

const char* wifiStatusReason(wl_status_t status) {
    switch (status) {
        case WL_NO_SSID_AVAIL:   return "SSID not found";
        case WL_CONNECT_FAILED:  return "auth failed";
        case WL_CONNECTION_LOST: return "link lost";
        case WL_DISCONNECTED:    return "disconnected";
        case WL_IDLE_STATUS:     return "idle";
        case WL_SCAN_COMPLETED:  return "scan complete";
        case WL_CONNECTED:       return "connected";
        default:                 return "unknown";
    }
}

void wifiClearLastFailure() {
    wifi_last_fail_ssid[0] = '\0';
    wifi_last_fail_reason[0] = '\0';
}

void wifiSetLastFailure(const char* ssid, const char* reason) {
    strncpy(wifi_last_fail_ssid, ssid, sizeof(wifi_last_fail_ssid) - 1);
    wifi_last_fail_ssid[sizeof(wifi_last_fail_ssid) - 1] = '\0';
    strncpy(wifi_last_fail_reason, reason, sizeof(wifi_last_fail_reason) - 1);
    wifi_last_fail_reason[sizeof(wifi_last_fail_reason) - 1] = '\0';
}

void wifiFormatFailureReason(const WiFiAttemptResult& result, char* out, size_t out_len) {
    if (result.timed_out) {
        snprintf(out, out_len, "timeout (%s)", wifiStatusReason(result.status));
    } else {
        snprintf(out, out_len, "%s", wifiStatusReason(result.status));
    }
}

int wifiConfigIndexForSSID(const String& ssid) {
    for (int i = 0; i < config_wifi_count; i++) {
        if (ssid == config_wifi[i].ssid) return i;
    }
    return -1;
}

// Try connecting to a single AP and capture final status/reason.
WiFiAttemptResult wifiTryAP(const char* ssid, const char* pass, int timeout_ms) {
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    if (pass && pass[0] != '\0') WiFi.begin(ssid, pass);
    else                         WiFi.begin(ssid);
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) return { true, st, false };
        if (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED || st == WL_CONNECTION_LOST) {
            return { false, st, false };
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        elapsed += 250;
    }
    wl_status_t final_status = WiFi.status();
    return { final_status == WL_CONNECTED, final_status, true };
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
            SERIAL_LOGF("WiFi: connected to %s, IP=%s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
            if (wifiSyncClockNtp()) SERIAL_LOGLN("Clock: NTP synced");
            else SERIAL_LOGLN("Clock: NTP sync failed");
        }
    } else if (wifi_state == WIFI_CONNECTED) {
        if (WiFi.status() != WL_CONNECTED) {
            wifi_state = WIFI_FAILED;
            SERIAL_LOGLN("WiFi: connection lost");
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
    ssh_last_host[0] = '\0';
    SERIAL_LOGLN("SSH: disconnected");
}

void sshReceiveTask(void* param);
void renderCommandPrompt();

bool hasNetwork() {
    return wifi_state == WIFI_CONNECTED;
}

void vpnDisconnect() {
    if (wg.is_initialized()) {
        wg.end();
    }
    vpn_connected = false;
}

bool vpnConnect(bool force_reinit) {
    if (vpnActive() && !force_reinit) return true;
    if (wg.is_initialized()) {
        connectMsg("VPN: reinit...");
        wg.end();
        vpn_connected = false;
    }
    if (!vpnConfigured()) {
        connectMsg("VPN: not configured");
        return false;
    }
    if (config_vpn_pubkey[0] == '\0' || config_vpn_ip[0] == '\0' || config_vpn_endpoint[0] == '\0' || config_vpn_port <= 0) {
        connectMsg("VPN: bad config");
        return false;
    }
    connectMsg("VPN: NTP sync...");
    if (!wifiSyncClockNtp()) {
        vpn_connected = false;
        connectMsg("VPN: NTP failed");
        return false;
    }
    connectMsg("VPN: connecting...");
    IPAddress local_ip;
    if (!local_ip.fromString(config_vpn_ip)) {
        vpn_connected = false;
        connectMsg("VPN: bad IP %s", config_vpn_ip);
        return false;
    }
    const char* psk = config_vpn_psk[0] ? config_vpn_psk : NULL;
    if (!wg.begin(local_ip, config_vpn_privkey, config_vpn_endpoint,
                  config_vpn_pubkey, config_vpn_port, psk)) {
        vpn_connected = false;
        connectMsg("VPN: connect failed");
        return false;
    }
    vpn_connected = true;
    if (config_vpn_dns[0] != '\0') {
        IPAddress dns_ip;
        if (dns_ip.fromString(config_vpn_dns)) {
            ip_addr_t dns_addr = IPADDR4_INIT(static_cast<uint32_t>(dns_ip));
            dns_setserver(0, &dns_addr);
            connectMsg("VPN DNS: %s", config_vpn_dns);
        } else {
            connectMsg("VPN DNS: bad %s", config_vpn_dns);
        }
    }
    connectMsg("VPN: %s", config_vpn_ip);
    return true;
}

bool sshTryConnect(const char* host) {
    if (!host || host[0] == '\0') {
        connectMsg("SSH: missing host");
        return false;
    }
    SERIAL_LOGF("SSH: connecting to %s:%d...\n", host, config_ssh_port);

    ssh_sess = ssh_new();
    if (!ssh_sess) return false;

    ssh_options_set(ssh_sess, SSH_OPTIONS_HOST, host);
    int port = config_ssh_port;
    ssh_options_set(ssh_sess, SSH_OPTIONS_PORT, &port);
    ssh_options_set(ssh_sess, SSH_OPTIONS_USER, config_ssh_user);
    long timeout = 5;  // 5 second connect timeout
    ssh_options_set(ssh_sess, SSH_OPTIONS_TIMEOUT, &timeout);

    if (ssh_connect(ssh_sess) != SSH_OK) {
        const char* err = ssh_get_error(ssh_sess);
        SERIAL_LOGF("SSH: connect failed: %s\n", err ? err : "(unknown)");
        if (err && strstr(err, "resolve hostname")) {
            connectMsg("SSH: DNS fail %s", host);
        }
        ssh_free(ssh_sess);
        ssh_sess = NULL;
        return false;
    }

    if (ssh_userauth_password(ssh_sess, NULL, config_ssh_pass) != SSH_AUTH_SUCCESS) {
        SERIAL_LOGF("SSH: auth failed: %s\n", ssh_get_error(ssh_sess));
        ssh_disconnect(ssh_sess);
        ssh_free(ssh_sess);
        ssh_sess = NULL;
        return false;
    }
    return true;
}

bool sshConnect() {
    if (!hasNetwork()) {
        SERIAL_LOGLN("SSH: no network");
        return false;
    }
    if (ssh_connected) {
        SERIAL_LOGLN("SSH: already connected");
        return false;
    }

    // Clean up any previous session
    sshDisconnect();

    const char* direct_host = config_ssh_host;
    const char* vpn_host = config_ssh_vpn_host[0] ? config_ssh_vpn_host : config_ssh_host;
    const bool vpn_only = vpnActive();

    if (vpn_only) {
        bool vpn_ssh_ok = false;
        connectMsg("SSH: %s (VPN)...", vpn_host);
        vpn_ssh_ok = sshTryConnect(vpn_host);
        if (!vpn_ssh_ok) {
            connectMsg("VPN: reinit...");
            if (vpnConnect(true)) {
                connectMsg("SSH: %s (VPN)...", vpn_host);
                vpn_ssh_ok = sshTryConnect(vpn_host);
            }
        }
        if (!vpn_ssh_ok) {
            connectMsg("SSH: failed via VPN");
            return false;
        }
        strncpy(ssh_last_host, vpn_host, sizeof(ssh_last_host) - 1);
        ssh_last_host[sizeof(ssh_last_host) - 1] = '\0';
        connectMsg("SSH: connected (VPN)");
    } else

    {
        // Try direct SSH first
        connectMsg("SSH: %s:%d...", direct_host, config_ssh_port);
        if (sshTryConnect(direct_host)) {
            strncpy(ssh_last_host, direct_host, sizeof(ssh_last_host) - 1);
            ssh_last_host[sizeof(ssh_last_host) - 1] = '\0';
            connectMsg("SSH: connected");
        } else if (vpnConfigured()) {
            // Direct failed, try VPN
            connectMsg("SSH: direct failed");
            if (!vpnConnect(false)) {
                connectMsg("SSH: VPN failed");
                return false;
            }

            bool vpn_ssh_ok = false;
            connectMsg("SSH: %s (VPN)...", vpn_host);
            vpn_ssh_ok = sshTryConnect(vpn_host);
            if (!vpn_ssh_ok && vpnActive()) {
                connectMsg("VPN: reinit...");
                if (vpnConnect(true)) {
                    connectMsg("SSH: %s (VPN)...", vpn_host);
                    vpn_ssh_ok = sshTryConnect(vpn_host);
                }
            }
            if (!vpn_ssh_ok) {
                connectMsg("SSH: failed via VPN");
                return false;
            }
            strncpy(ssh_last_host, vpn_host, sizeof(ssh_last_host) - 1);
            ssh_last_host[sizeof(ssh_last_host) - 1] = '\0';
            connectMsg("SSH: connected (VPN)");
        } else {
            connectMsg("SSH: failed");
            return false;
        }
    }

    ssh_chan = ssh_channel_new(ssh_sess);
    if (!ssh_chan) {
        SERIAL_LOGLN("SSH: channel_new failed");
        ssh_disconnect(ssh_sess);
        ssh_free(ssh_sess);
        ssh_sess = NULL;

        return false;
    }

    if (ssh_channel_open_session(ssh_chan) != SSH_OK) {
        SERIAL_LOGF("SSH: channel open failed: %s\n", ssh_get_error(ssh_sess));
        ssh_channel_free(ssh_chan);
        ssh_chan = NULL;
        ssh_disconnect(ssh_sess);
        ssh_free(ssh_sess);
        ssh_sess = NULL;

        return false;
    }

    // Request PTY sized to our screen
    if (ssh_channel_request_pty_size(ssh_chan, "xterm", TERM_COLS, ROWS_PER_SCREEN) != SSH_OK) {
        SERIAL_LOGF("SSH: pty request failed: %s\n", ssh_get_error(ssh_sess));
        ssh_channel_close(ssh_chan);
        ssh_channel_free(ssh_chan);
        ssh_chan = NULL;
        ssh_disconnect(ssh_sess);
        ssh_free(ssh_sess);
        ssh_sess = NULL;

        return false;
    }

    if (ssh_channel_request_shell(ssh_chan) != SSH_OK) {
        SERIAL_LOGF("SSH: shell request failed: %s\n", ssh_get_error(ssh_sess));
        ssh_channel_close(ssh_chan);
        ssh_channel_free(ssh_chan);
        ssh_chan = NULL;
        ssh_disconnect(ssh_sess);
        ssh_free(ssh_sess);
        ssh_sess = NULL;

        return false;
    }

    ssh_connected = true;
    SERIAL_LOGLN("SSH: connected!");

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
        wifiClearLastFailure();

        if (config_wifi_count == 0) {
            connectMsg("WiFi: no APs in /CONFIG");
        } else {
            // Try each known AP in config order.
            for (int i = 0; i < config_wifi_count && !connected; i++) {
                connectMsg("WiFi: %s...", config_wifi[i].ssid);
                WiFiAttemptResult attempt = wifiTryAP(config_wifi[i].ssid, config_wifi[i].pass, WIFI_CONNECT_TIMEOUT_MS);
                if (attempt.connected) {
                    connected = true;
                    wifiClearLastFailure();
                } else {
                    char reason[48];
                    wifiFormatFailureReason(attempt, reason, sizeof(reason));
                    connectMsg("  failed: %s", reason);
                    wifiSetLastFailure(config_wifi[i].ssid, reason);
                }
            }
        }

        if (connected) {
            wifi_state = WIFI_CONNECTED;
            connectMsg("WiFi: %s (%s)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
            connectMsg("Clock: NTP sync...");
            if (wifiSyncClockNtp()) connectMsg("Clock: NTP synced");
            else connectMsg("Clock: NTP failed");
        } else {
            wifi_state = WIFI_FAILED;
            connectMsg("WiFi: all failed");
            if (wifi_last_fail_ssid[0] != '\0') {
                connectMsg("Last: %s", wifi_last_fail_ssid);
                connectMsg("Why: %s", wifi_last_fail_reason);
            }
            connectMsg("Use cmd: scan");
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
            SERIAL_LOGLN("SSH: channel closed by remote");
            ssh_connected = false;

            // Reset parser/buffer immediately so stale TUI content does not linger.
            xSemaphoreTake(state_mutex, portMAX_DELAY);
            terminalClear();
            xSemaphoreGive(state_mutex);

            connect_status_count = 0;
            partial_count = 100;  // force a full clean redraw on next terminal render
            term_render_requested = true;
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}
