// --- BLE input receiver: laptop -> device keystroke stream over a GATT service ---
//
// On the reTerminal there is no physical keyboard. A small laptop companion
// (scripts/companion.py) connects over BLE and writes terminal-style bytes to a
// Nordic-UART-style service; we feed them into the current app mode (notepad /
// terminal / command). A separate control characteristic carries mode commands
// (open the command palette, or run a command directly).
//
// This module deliberately exposes the same status surface as bluetooth_module
// (btIsConnected/btIsEnabled/btStatusShort/btPeerAddress/btIsBonded/btPoll) so
// the shared rendering and status code compiles unchanged on either board.

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <esp_gap_ble_api.h>
#include "freertos/stream_buffer.h"
#include <cstring>
#include <strings.h>
#include <string>

// Nordic UART Service UUIDs (well supported by generic BLE tooling), plus a
// fourth characteristic for control messages.
#define BLEIN_SERVICE_UUID  "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLEIN_RX_UUID       "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // laptop -> device keystrokes
#define BLEIN_TX_UUID       "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // device -> laptop status (notify)
#define BLEIN_CTRL_UUID     "6e400004-b5a3-f393-e0a9-e50e24dcca9e"  // laptop -> device control

#define BLEIN_STREAM_BYTES   1024
#define BLEIN_CTRL_BYTES     256

// Forward declarations — defined later in keyboard_module.hpp / cli_module.hpp
// (all part of the same translation unit via main.cpp's include order).
bool editorInsertChar(char c);
bool editorBackspace();
void cursorUp();
void cursorDown();
void cursorLeft();
void cursorRight();
bool cmdInsertChar(char c);
bool cmdBackspace();
bool cmdSubmit();
bool executeCommand(const char* cmd);

enum BtState { BT_STATE_OFF, BT_STATE_ADVERTISING, BT_STATE_CONNECTED, BT_STATE_ERROR };

static volatile BtState bt_state = BT_STATE_OFF;
static volatile bool bt_initialized = false;
static volatile bool bt_connected = false;
static volatile bool bt_advertising = false;
static volatile bool bt_bonded = false;
static char bt_peer_addr[18] = "";

static BLEServer* blein_server = NULL;
static BLEService* blein_service = NULL;
static BLECharacteristic* blein_rx = NULL;
static BLECharacteristic* blein_tx = NULL;
static BLECharacteristic* blein_ctrl = NULL;

static StreamBufferHandle_t blein_rx_stream = NULL;
static StreamBufferHandle_t blein_ctrl_stream = NULL;

// --- Status surface (compatible with bluetooth_module) ---

const char* btStatusShort() {
    if (!config_bt_enabled) return "off";
    if (bt_state == BT_STATE_ERROR) return "err";
    if (bt_connected) return "ok";
    if (bt_advertising) return "adv";
    if (bt_initialized) return "idle";
    return "off";
}
const char* btPeerAddress() { return bt_peer_addr; }
bool btIsConnected() { return bt_connected; }
bool btIsEnabled() { return config_bt_enabled; }
bool btIsBonded() { return bt_bonded; }

static void btFormatAddr(const uint8_t* addr, char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (!addr) { out[0] = '\0'; return; }
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

// --- GATT callbacks ---

class BleInServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* s) override {
        (void)s;
        bt_connected = true;
        bt_advertising = false;
        bt_state = BT_STATE_CONNECTED;
        render_requested = true;
        term_render_requested = true;
        SERIAL_LOGLN("BLEIN: client connected");
    }
    void onConnect(BLEServer* s, esp_ble_gatts_cb_param_t* param) override {
        onConnect(s);
        if (param) {
            btFormatAddr(param->connect.remote_bda, bt_peer_addr, sizeof(bt_peer_addr));
            SERIAL_LOGF("BLEIN: peer=%s\n", bt_peer_addr);
            esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_NO_MITM);
        }
    }
    void onDisconnect(BLEServer* s) override {
        (void)s;
        bt_connected = false;
        bt_advertising = false;
        bt_state = BT_STATE_OFF;
        render_requested = true;
        term_render_requested = true;
        SERIAL_LOGLN("BLEIN: client disconnected");
    }
};

class BleInRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        std::string v = c->getValue();
        if (!v.empty() && blein_rx_stream) {
            xStreamBufferSend(blein_rx_stream, v.data(), v.size(), 0);
        }
    }
};

class BleInCtrlCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        std::string v = c->getValue();
        if (!v.empty() && blein_ctrl_stream) {
            xStreamBufferSend(blein_ctrl_stream, v.data(), v.size(), 0);
            char nl = '\n';   // newline-frame each control message
            xStreamBufferSend(blein_ctrl_stream, &nl, 1, 0);
        }
    }
};

class BleInSecurityCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t pass_key) override { (void)pass_key; }
    bool onSecurityRequest() override { return true; }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
        bt_bonded = cmpl.success;
        btFormatAddr(cmpl.bd_addr, bt_peer_addr, sizeof(bt_peer_addr));
        if (cmpl.success) SERIAL_LOGF("BLEIN: paired with %s\n", bt_peer_addr);
        else SERIAL_LOGF("BLEIN: pair failed reason=0x%02X\n", cmpl.fail_reason);
        render_requested = true;
        term_render_requested = true;
    }
    bool onConfirmPIN(uint32_t pin) override { (void)pin; return true; }
};

static BleInServerCallbacks blein_server_cbs;
static BleInRxCallbacks blein_rx_cbs;
static BleInCtrlCallbacks blein_ctrl_cbs;
static BleInSecurityCallbacks blein_sec_cbs;
static BLESecurity blein_security;

static void bleInputStartAdvertising() {
    if (!bt_initialized || !config_bt_enabled || bt_connected) return;
    BLEDevice::startAdvertising();
    bt_advertising = true;
    bt_state = BT_STATE_ADVERTISING;
    SERIAL_LOGLN("BLEIN: advertising");
}

void bleInputInit() {
    if (bt_initialized) return;
    config_bt_enabled = true;

    if (!blein_rx_stream)   blein_rx_stream   = xStreamBufferCreate(BLEIN_STREAM_BYTES, 1);
    if (!blein_ctrl_stream) blein_ctrl_stream = xStreamBufferCreate(BLEIN_CTRL_BYTES, 1);

    BLEDevice::init(config_bt_name);
    BLEDevice::setSecurityCallbacks(&blein_sec_cbs);
    blein_security.setCapability(ESP_IO_CAP_NONE);
    blein_security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    blein_security.setKeySize(16);
    blein_security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    blein_security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    blein_server = BLEDevice::createServer();
    if (!blein_server) { bt_state = BT_STATE_ERROR; SERIAL_LOGLN("BLEIN: createServer failed"); return; }
    blein_server->setCallbacks(&blein_server_cbs);

    blein_service = blein_server->createService(BLEIN_SERVICE_UUID);
    if (!blein_service) { bt_state = BT_STATE_ERROR; SERIAL_LOGLN("BLEIN: createService failed"); return; }

    blein_rx = blein_service->createCharacteristic(
        BLEIN_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    blein_rx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
    blein_rx->setCallbacks(&blein_rx_cbs);

    blein_ctrl = blein_service->createCharacteristic(
        BLEIN_CTRL_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    blein_ctrl->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
    blein_ctrl->setCallbacks(&blein_ctrl_cbs);

    blein_tx = blein_service->createCharacteristic(
        BLEIN_TX_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    blein_tx->addDescriptor(new BLE2902());
    blein_tx->setValue("notepad");

    blein_service->start();

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    if (adv) {
        adv->addServiceUUID(BLEIN_SERVICE_UUID);
        adv->setScanResponse(true);
        adv->setMinPreferred(0x06);
        adv->setMaxPreferred(0x12);
    }

    bt_initialized = true;
    bt_connected = false;
    bt_advertising = false;
    bt_bonded = false;
    bt_peer_addr[0] = '\0';

    bleInputStartAdvertising();
    SERIAL_LOGF("BLEIN: ready name=%s (encrypted bonding)\n", config_bt_name);
}

void bleInputPoll() {
    if (!config_bt_enabled || !bt_initialized) return;
    if (!bt_connected && !bt_advertising) bleInputStartAdvertising();
}

// Shared callers use btPoll(); on this board it drives the input link.
void btPoll() { bleInputPoll(); }

static void bleInputPublishMode() {
    if (!blein_tx || !bt_connected) return;
    AppMode a = app_mode;
    const char* m = (a == MODE_TERMINAL) ? "terminal" : (a == MODE_COMMAND) ? "command" : "notepad";
    blein_tx->setValue(m);
    blein_tx->notify();
}

// Feed received keystroke bytes into the current app mode. Caller holds state_mutex.
static void bleInputFeedLocked(const uint8_t* data, int len) {
    AppMode mode = app_mode;

    if (mode == MODE_TERMINAL) {
        // Pass the raw VT byte stream straight to SSH (arrows / Ctrl / Esc are
        // their own byte sequences — full keyboard fidelity).
        sshSendString((const char*)data, len);
        return;
    }

    bool changed = false;
    for (int i = 0; i < len; i++) {
        uint8_t c = data[i];

        // Arrow keys arrive as CSI sequences: ESC [ A/B/C/D
        if (c == 0x1B && i + 2 < len && data[i + 1] == '[') {
            char dir = (char)data[i + 2];
            i += 2;
            if (mode == MODE_NOTEPAD) {
                if (dir == 'A') cursorUp();
                else if (dir == 'B') cursorDown();
                else if (dir == 'C') cursorRight();
                else if (dir == 'D') cursorLeft();
                changed = true;
            }
            continue;
        }

        if (mode == MODE_NOTEPAD) {
            if (c == '\r' || c == '\n')      changed |= editorInsertChar('\n');
            else if (c == 0x08 || c == 0x7F) changed |= editorBackspace();
            else if (c >= ' ' && c <= '~')   changed |= editorInsertChar((char)c);
        } else if (mode == MODE_COMMAND) {
            if (c == '\r' || c == '\n')      { cmdSubmit(); changed = true; }
            else if (c == 0x08 || c == 0x7F) changed |= cmdBackspace();
            else if (c >= ' ' && c <= '~')   changed |= cmdInsertChar((char)c);
        }
    }

    if (changed) {
        if (app_mode == MODE_TERMINAL) term_render_requested = true;
        else render_requested = true;
    }
}

// Process one control message. Caller holds state_mutex.
static void bleInputControlLocked(const char* msg) {
    if (!msg || !*msg) return;

    if (strcasecmp(msg, "OPEN") == 0) {
        // Open the command palette (mirrors a MIC single-tap on the T-Deck).
        cmd_return_mode = app_mode;
        cmd_len = 0;
        cmd_buf[0] = '\0';
        cmdHistoryResetBrowseLocked();
        cmd_result_valid = false;
        cmdPickerStop();
        app_mode = MODE_COMMAND;
        render_requested = true;
        return;
    }

    if (strncasecmp(msg, "CMD ", 4) == 0) {
        cmd_return_mode = app_mode;
        xSemaphoreGive(state_mutex);
        executeCommand(msg + 4);
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        if (app_mode == MODE_TERMINAL) term_render_requested = true;
        else render_requested = true;
    }
}

// Drain queued BLE bytes / control messages — call once per loop().
void bleInputDrain() {
    if (!bt_initialized) return;

    if (blein_rx_stream && xStreamBufferBytesAvailable(blein_rx_stream) > 0) {
        uint8_t buf[128];
        size_t n = xStreamBufferReceive(blein_rx_stream, buf, sizeof(buf), 0);
        if (n > 0 && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(25)) == pdTRUE) {
            bleInputFeedLocked(buf, (int)n);
            xSemaphoreGive(state_mutex);
        }
    }

    if (blein_ctrl_stream && xStreamBufferBytesAvailable(blein_ctrl_stream) > 0) {
        static char line[BLEIN_CTRL_BYTES];
        static int line_len = 0;
        uint8_t b;
        while (xStreamBufferReceive(blein_ctrl_stream, &b, 1, 0) == 1) {
            if (b == '\n') {
                line[line_len] = '\0';
                if (line_len > 0 && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(25)) == pdTRUE) {
                    bleInputControlLocked(line);
                    xSemaphoreGive(state_mutex);
                }
                line_len = 0;
            } else if (line_len < (int)sizeof(line) - 1) {
                line[line_len++] = (char)b;
            }
        }
    }

    bleInputPublishMode();
}
