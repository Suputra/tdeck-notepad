#pragma once

#include <Arduino.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>

#define MODEM_BAUDRATE               115200U
#define MODEM_BOOT_WAIT_MS           3200U
#define MODEM_AT_WAIT_MS             5000U
#define MODEM_SCAN_DEFAULT_DWELL_MS  4000U
#define MODEM_SCAN_MIN_DWELL_MS      1000U
#define MODEM_SCAN_SAMPLE_MS         700U
#define MODEM_TASK_STACK_BYTES       6144U

enum ModemState {
    MODEM_STATE_OFF,
    MODEM_STATE_BOOTING,
    MODEM_STATE_ON,
    MODEM_STATE_SCANNING,
    MODEM_STATE_ERROR
};

enum ModemJobType {
    MODEM_JOB_NONE,
    MODEM_JOB_POWER_ON,
    MODEM_JOB_SCAN
};

struct ModemScanResult {
    bool ok;
    bool had_at;
    bool sim_ready;
    int csq_last;         // 0..31, 99=unknown
    int csq_best;         // best observed during dwell
    int rssi_dbm_last;    // converted from CSQ, -999 unknown
    int rssi_dbm_best;    // converted from CSQ, -999 unknown
    uint32_t elapsed_ms;
    char cpin[24];        // +CPIN line summary
    char cereg[48];       // +CEREG line summary
    char cpsi[96];        // +CPSI line summary
    char cops[96];        // +COPS line summary
    char error[64];       // local failure reason
};

static HardwareSerial& modem_serial = Serial1;
static volatile ModemState modem_state = MODEM_STATE_OFF;
static volatile ModemJobType modem_job = MODEM_JOB_NONE;
static TaskHandle_t modem_task_handle = NULL;
static bool modem_uart_started = false;

static bool modem_scan_running = false;
static bool modem_scan_done = false;
static bool modem_scan_restore_off = false;
static uint32_t modem_scan_dwell_ms = MODEM_SCAN_DEFAULT_DWELL_MS;
static ModemScanResult modem_scan_result = {};
static char modem_last_error[64] = "";
static bool modem_power_event_pending = false;
static bool modem_power_event_ok = false;

static void modemMarkRenderDirty() {
    render_requested = true;
    term_render_requested = true;
}

static bool modemResponseHasOk(const String& response) {
    return response.indexOf("OK") >= 0;
}

static void modemCopyLineValue(char* out, size_t out_len, const String& response, const char* tag) {
    if (!out || out_len < 2) return;
    out[0] = '\0';
    if (!tag || !*tag) return;

    int idx = response.indexOf(tag);
    if (idx < 0) return;

    int end = response.indexOf('\n', idx);
    if (end < 0) end = response.length();
    String line = response.substring(idx, end);
    line.replace("\r", "");
    line.trim();
    snprintf(out, out_len, "%s", line.c_str());
}

static String modemSendAT(const char* cmd, uint32_t timeout_ms = 1500, bool stop_on_ok_or_error = true) {
    while (modem_serial.available()) modem_serial.read();
    modem_serial.println(cmd);

    uint32_t start = millis();
    String response;
    response.reserve(192);
    while ((uint32_t)(millis() - start) < timeout_ms) {
        while (modem_serial.available()) {
            response += (char)modem_serial.read();
        }
        if (stop_on_ok_or_error && (response.indexOf("OK") >= 0 || response.indexOf("ERROR") >= 0)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return response;
}

static int modemParseCsq(const String& response) {
    int idx = response.indexOf("+CSQ:");
    if (idx < 0) return -1;

    idx = response.indexOf(':', idx);
    if (idx < 0) return -1;
    idx++;

    while (idx < response.length() && isspace((unsigned char)response[idx])) idx++;

    int value = 0;
    bool any = false;
    while (idx < response.length() && isdigit((unsigned char)response[idx])) {
        any = true;
        value = value * 10 + (response[idx] - '0');
        idx++;
    }
    if (!any || value < 0 || value > 99) return -1;
    return value;
}

static int modemCsqToDbm(int csq) {
    if (csq < 0 || csq == 99 || csq > 31) return -999;
    return -113 + (2 * csq);
}

static bool modemWaitATReady(uint32_t timeout_ms = MODEM_AT_WAIT_MS) {
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < timeout_ms) {
        String r = modemSendAT("AT", 700);
        if (modemResponseHasOk(r)) return true;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return false;
}

static void modemApplyPowerOff(bool graceful) {
    if (graceful) {
        // PWRKEY long pulse requests orderly modem shutdown.
        digitalWrite(BOARD_MODEM_PWRKEY, LOW);
        vTaskDelay(pdMS_TO_TICKS(10));
        digitalWrite(BOARD_MODEM_PWRKEY, HIGH);
        vTaskDelay(pdMS_TO_TICKS(3000));
        digitalWrite(BOARD_MODEM_PWRKEY, LOW);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        digitalWrite(BOARD_MODEM_PWRKEY, LOW);
    }

    digitalWrite(BOARD_MODEM_POWER_EN, LOW);
    if (modem_uart_started) {
        modem_serial.end();
        modem_uart_started = false;
    }
    modem_state = MODEM_STATE_OFF;
}

static void modemResetScanResult() {
    memset(&modem_scan_result, 0, sizeof(modem_scan_result));
    modem_scan_result.csq_last = -1;
    modem_scan_result.csq_best = -1;
    modem_scan_result.rssi_dbm_last = -999;
    modem_scan_result.rssi_dbm_best = -999;
}

static bool modemPowerOnBlocking() {
    modem_state = MODEM_STATE_BOOTING;

    if (!modem_uart_started) {
        // HardwareSerial.begin signature: begin(baud, config, rxPin, txPin).
        // BOARD_MODEM_TXD is modem->ESP (RX), BOARD_MODEM_RXD is ESP->modem (TX).
        modem_serial.begin(MODEM_BAUDRATE, SERIAL_8N1, BOARD_MODEM_TXD, BOARD_MODEM_RXD);
        modem_uart_started = true;
    }

    digitalWrite(BOARD_MODEM_POWER_EN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Short PWRKEY pulse = power on
    digitalWrite(BOARD_MODEM_PWRKEY, LOW);
    vTaskDelay(pdMS_TO_TICKS(10));
    digitalWrite(BOARD_MODEM_PWRKEY, HIGH);
    vTaskDelay(pdMS_TO_TICKS(50));
    digitalWrite(BOARD_MODEM_PWRKEY, LOW);
    vTaskDelay(pdMS_TO_TICKS(10));

    vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_WAIT_MS));

    if (!modemWaitATReady()) {
        snprintf(modem_last_error, sizeof(modem_last_error), "AT timeout");
        modemApplyPowerOff(false);
        modem_state = MODEM_STATE_ERROR;
        return false;
    }

    modemSendAT("ATE0", 1000);
    modem_state = MODEM_STATE_ON;
    modem_last_error[0] = '\0';
    return true;
}

static void modemRunScanBlocking() {
    bool restore_off = modem_scan_restore_off;
    uint32_t dwell_ms = modem_scan_dwell_ms;
    if (dwell_ms < MODEM_SCAN_MIN_DWELL_MS) dwell_ms = MODEM_SCAN_MIN_DWELL_MS;

    modem_state = MODEM_STATE_SCANNING;

    if (restore_off && !modemPowerOnBlocking()) {
        snprintf(modem_scan_result.error, sizeof(modem_scan_result.error),
                 "%s", modem_last_error[0] ? modem_last_error : "power on failed");
        modem_scan_running = false;
        modem_scan_done = true;
        return;
    }

    String ping = modemSendAT("AT", 800);
    modem_scan_result.had_at = modemResponseHasOk(ping);
    if (!modem_scan_result.had_at) {
        snprintf(modem_scan_result.error, sizeof(modem_scan_result.error), "AT no response");
        snprintf(modem_last_error, sizeof(modem_last_error), "%s", modem_scan_result.error);
        modem_scan_running = false;
        modem_scan_done = true;
        if (restore_off) modemApplyPowerOff(false);
        modem_state = MODEM_STATE_ERROR;
        return;
    }

    String cpin = modemSendAT("AT+CPIN?", 1000);
    modemCopyLineValue(modem_scan_result.cpin, sizeof(modem_scan_result.cpin), cpin, "+CPIN:");
    modem_scan_result.sim_ready = cpin.indexOf("READY") >= 0;

    String cereg = modemSendAT("AT+CEREG?", 1000);
    modemCopyLineValue(modem_scan_result.cereg, sizeof(modem_scan_result.cereg), cereg, "+CEREG:");

    String cpsi = modemSendAT("AT+CPSI?", 1200);
    modemCopyLineValue(modem_scan_result.cpsi, sizeof(modem_scan_result.cpsi), cpsi, "+CPSI:");

    String cops = modemSendAT("AT+COPS?", 1200);
    modemCopyLineValue(modem_scan_result.cops, sizeof(modem_scan_result.cops), cops, "+COPS:");

    uint32_t started_ms = millis();
    uint32_t next_sample_ms = 0;
    while ((uint32_t)(millis() - started_ms) < dwell_ms) {
        uint32_t now = millis();
        if (now >= next_sample_ms) {
            next_sample_ms = now + MODEM_SCAN_SAMPLE_MS;
            String csq_resp = modemSendAT("AT+CSQ", 1000);
            int csq = modemParseCsq(csq_resp);
            if (csq >= 0) {
                modem_scan_result.csq_last = csq;
                modem_scan_result.rssi_dbm_last = modemCsqToDbm(csq);
                if (modem_scan_result.csq_best < 0 ||
                    (csq != 99 && modem_scan_result.csq_best == 99) ||
                    csq > modem_scan_result.csq_best) {
                    modem_scan_result.csq_best = csq;
                    modem_scan_result.rssi_dbm_best = modemCsqToDbm(csq);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    modem_scan_result.elapsed_ms = millis() - started_ms;
    modem_scan_result.ok = modem_scan_result.had_at && modem_scan_result.csq_last >= 0;
    if (!modem_scan_result.ok && modem_scan_result.error[0] == '\0') {
        snprintf(modem_scan_result.error, sizeof(modem_scan_result.error), "no CSQ");
        snprintf(modem_last_error, sizeof(modem_last_error), "%s", modem_scan_result.error);
    } else if (modem_scan_result.ok) {
        modem_last_error[0] = '\0';
    }

    modem_scan_running = false;
    modem_scan_done = true;

    if (restore_off) {
        modemApplyPowerOff(false);
    } else {
        modem_state = MODEM_STATE_ON;
    }
}

static void modemWorkerTask(void* param) {
    ModemJobType job = (ModemJobType)(uintptr_t)param;

    if (job == MODEM_JOB_POWER_ON) {
        bool ok = modemPowerOnBlocking();
        modem_power_event_ok = ok;
        modem_power_event_pending = true;
    } else if (job == MODEM_JOB_SCAN) {
        modemRunScanBlocking();
    }

    modem_job = MODEM_JOB_NONE;
    modem_task_handle = NULL;
    modemMarkRenderDirty();
    vTaskDelete(NULL);
}

static bool modemStartWorker(ModemJobType job) {
    if (modem_job != MODEM_JOB_NONE || modem_task_handle != NULL) {
        snprintf(modem_last_error, sizeof(modem_last_error), "busy");
        return false;
    }

    modem_job = job;
    BaseType_t ok = xTaskCreatePinnedToCore(
        modemWorkerTask,
        (job == MODEM_JOB_SCAN) ? "modem_scan" : "modem_power",
        MODEM_TASK_STACK_BYTES,
        (void*)(uintptr_t)job,
        1,
        &modem_task_handle,
        1
    );
    if (ok != pdPASS) {
        modem_job = MODEM_JOB_NONE;
        modem_task_handle = NULL;
        snprintf(modem_last_error, sizeof(modem_last_error), "task start failed");
        return false;
    }
    return true;
}

void modemInit() {
    pinMode(BOARD_MODEM_POWER_EN, OUTPUT);
    pinMode(BOARD_MODEM_PWRKEY, OUTPUT);
    pinMode(BOARD_MODEM_RST, OUTPUT);
    pinMode(BOARD_MODEM_DTR, OUTPUT);

    digitalWrite(BOARD_MODEM_POWER_EN, LOW);
    digitalWrite(BOARD_MODEM_PWRKEY, LOW);
    digitalWrite(BOARD_MODEM_RST, HIGH);
    digitalWrite(BOARD_MODEM_DTR, LOW);

    modem_state = MODEM_STATE_OFF;
    modem_job = MODEM_JOB_NONE;
    modem_task_handle = NULL;
    modem_uart_started = false;
    modem_scan_running = false;
    modem_scan_done = false;
    modem_scan_restore_off = false;
    modem_scan_dwell_ms = MODEM_SCAN_DEFAULT_DWELL_MS;
    modem_last_error[0] = '\0';
    modem_power_event_pending = false;
    modem_power_event_ok = false;
    modemResetScanResult();
}

bool modemIsPowered() {
    return modem_state == MODEM_STATE_ON || modem_state == MODEM_STATE_SCANNING || modem_state == MODEM_STATE_BOOTING;
}

bool modemOperationInProgress() {
    return modem_job != MODEM_JOB_NONE;
}

ModemState modemGetState() {
    return modem_state;
}

const char* modemStatusShort() {
    if (modem_scan_running) return "scan";
    if (modem_job == MODEM_JOB_POWER_ON) return "boot";

    switch (modem_state) {
        case MODEM_STATE_OFF: return "off";
        case MODEM_STATE_BOOTING: return "boot";
        case MODEM_STATE_ON: return "on";
        case MODEM_STATE_SCANNING: return "scan";
        case MODEM_STATE_ERROR: return "err";
        default: return "off";
    }
}

const char* modemLastError() {
    return modem_last_error;
}

int modemLastCsq() {
    return modem_scan_result.csq_last;
}

bool modemSetPowered(bool on) {
    if (on) {
        if (modemIsPowered()) {
            snprintf(modem_last_error, sizeof(modem_last_error), "already on");
            return false;
        }
        if (modem_scan_running) {
            snprintf(modem_last_error, sizeof(modem_last_error), "scan running");
            return false;
        }
        modem_state = MODEM_STATE_BOOTING;
        modemMarkRenderDirty();
        return modemStartWorker(MODEM_JOB_POWER_ON);
    }

    if (modem_scan_running) {
        snprintf(modem_last_error, sizeof(modem_last_error), "scan running");
        return false;
    }
    if (modemOperationInProgress()) {
        snprintf(modem_last_error, sizeof(modem_last_error), "busy");
        return false;
    }
    if (modem_state == MODEM_STATE_OFF) {
        snprintf(modem_last_error, sizeof(modem_last_error), "already off");
        return false;
    }

    // Keep turn-off immediate to avoid blocking UI/loop.
    modemApplyPowerOff(false);
    modem_last_error[0] = '\0';
    modemMarkRenderDirty();
    return true;
}

bool modemScanStartAsync(uint32_t dwell_ms) {
    if (dwell_ms < MODEM_SCAN_MIN_DWELL_MS) dwell_ms = MODEM_SCAN_MIN_DWELL_MS;
    if (modem_scan_running) {
        snprintf(modem_last_error, sizeof(modem_last_error), "scan running");
        return false;
    }
    if (modemOperationInProgress()) {
        snprintf(modem_last_error, sizeof(modem_last_error), "busy");
        return false;
    }

    modemResetScanResult();
    modem_scan_running = true;
    modem_scan_done = false;
    modem_scan_restore_off = !modemIsPowered();
    modem_scan_dwell_ms = dwell_ms;
    modem_state = MODEM_STATE_SCANNING;
    modemMarkRenderDirty();

    if (!modemStartWorker(MODEM_JOB_SCAN)) {
        modem_scan_running = false;
        modem_scan_done = false;
        if (modem_state == MODEM_STATE_SCANNING) {
            modem_state = modem_scan_restore_off ? MODEM_STATE_OFF : MODEM_STATE_ON;
        }
        modemMarkRenderDirty();
        return false;
    }
    return true;
}

void modemPoll() {
    // Worker-task model: no per-loop work required.
}

bool modemScanInProgress() {
    return modem_scan_running;
}

bool modemScanTakeResult(ModemScanResult* out) {
    if (!modem_scan_done || modem_scan_running) return false;
    if (out) *out = modem_scan_result;
    modem_scan_done = false;
    return true;
}

bool modemTakePowerOnEvent(bool* out_ok) {
    if (!modem_power_event_pending) return false;
    if (out_ok) *out_ok = modem_power_event_ok;
    modem_power_event_pending = false;
    return true;
}
