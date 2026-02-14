// --- Bluetooth LE barebones peripheral (no HID keyboard reports) ---

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <BLEUtils.h>
#include <esp_gap_ble_api.h>
#include <cstring>

#define BT_PASSKEY_MIN           100000U
#define BT_PASSKEY_MAX           999999U
#define BT_ADV_TIMEOUT_MS        45000U

#define BT_SERVICE_UUID          "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BT_STATUS_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"

enum BtState { BT_STATE_OFF, BT_STATE_ADVERTISING, BT_STATE_CONNECTED, BT_STATE_ERROR };

static volatile BtState bt_state = BT_STATE_OFF;
static volatile bool bt_initialized = false;
static volatile bool bt_connected = false;
static volatile bool bt_advertising = false;
static volatile bool bt_bonded = false;
static unsigned long bt_adv_started_at = 0;

static BLEServer* bt_server = NULL;
static BLEService* bt_service = NULL;
static BLECharacteristic* bt_status_char = NULL;

static char bt_peer_addr[18] = "";

static void btFormatAddr(const uint8_t* addr, char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (!addr) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static bool btPasskeyConfigured() {
    return config_bt_passkey >= BT_PASSKEY_MIN && config_bt_passkey <= BT_PASSKEY_MAX;
}

static esp_gatt_perm_t btReadPerm() {
    return btPasskeyConfigured() ? ESP_GATT_PERM_READ_ENC_MITM : ESP_GATT_PERM_READ_ENCRYPTED;
}

static void btSetStatusValue(const char* value) {
    if (!bt_status_char) return;
    bt_status_char->setValue(value ? value : "idle");
}

const char* btStatusShort() {
    if (!config_bt_enabled) return "off";
    if (bt_state == BT_STATE_ERROR) return "err";
    if (bt_connected) return "ok";
    if (bt_advertising) return "adv";
    if (bt_initialized) return "idle";
    return "off";
}

const char* btPeerAddress() {
    return bt_peer_addr;
}

bool btIsConnected() {
    return bt_connected;
}

bool btIsEnabled() {
    return config_bt_enabled;
}

bool btIsBonded() {
    return bt_bonded;
}

// HID input is removed in barebones mode.
bool btSendUsage(uint8_t usage, uint8_t modifiers) {
    (void)usage;
    (void)modifiers;
    return false;
}

bool btTypeChar(char c, uint8_t extra_modifiers) {
    (void)c;
    (void)extra_modifiers;
    return false;
}

size_t btTypeTextN(const char* text, size_t len, uint8_t extra_modifiers) {
    (void)text;
    (void)len;
    (void)extra_modifiers;
    return 0;
}

size_t btTypeText(const char* text) {
    (void)text;
    return 0;
}

class BtServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        (void)pServer;
        bt_connected = true;
        bt_advertising = false;
        bt_state = BT_STATE_CONNECTED;
        btSetStatusValue("connected");
        render_requested = true;
        term_render_requested = true;
        SERIAL_LOGLN("BT: client connected");
    }

    void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
        onConnect(pServer);
        if (param) {
            btFormatAddr(param->connect.remote_bda, bt_peer_addr, sizeof(bt_peer_addr));
            SERIAL_LOGF("BT: peer=%s\n", bt_peer_addr);
            esp_ble_sec_act_t sec_act = btPasskeyConfigured() ? ESP_BLE_SEC_ENCRYPT_MITM : ESP_BLE_SEC_ENCRYPT_NO_MITM;
            esp_err_t err = esp_ble_set_encryption(param->connect.remote_bda, sec_act);
            if (err != ESP_OK) {
                SERIAL_LOGF("BT: set_encryption failed err=%d\n", (int)err);
            }
        }
    }

    void onDisconnect(BLEServer* pServer) override {
        (void)pServer;
        bt_connected = false;
        bt_advertising = false;
        bt_state = BT_STATE_OFF;
        btSetStatusValue("idle");
        render_requested = true;
        term_render_requested = true;
        SERIAL_LOGLN("BT: client disconnected");
    }

    void onDisconnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
        onDisconnect(pServer);
        if (param) {
            btFormatAddr(param->disconnect.remote_bda, bt_peer_addr, sizeof(bt_peer_addr));
        }
    }
};

class BtSecurityCallbacks : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override {
        return btPasskeyConfigured() ? config_bt_passkey : 123456;
    }

    void onPassKeyNotify(uint32_t pass_key) override {
        SERIAL_LOGF("BT: passkey notify %06u\n", (unsigned)pass_key);
    }

    bool onSecurityRequest() override {
        return true;
    }

    void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) override {
        bt_bonded = auth_cmpl.success;
        btFormatAddr(auth_cmpl.bd_addr, bt_peer_addr, sizeof(bt_peer_addr));
        if (auth_cmpl.success) {
            SERIAL_LOGF("BT: paired with %s\n", bt_peer_addr);
        } else {
            SERIAL_LOGF("BT: pair failed reason=0x%02X\n", auth_cmpl.fail_reason);
        }
        render_requested = true;
        term_render_requested = true;
    }

    bool onConfirmPIN(uint32_t pin) override {
        SERIAL_LOGF("BT: confirm pin %06u\n", (unsigned)pin);
        return true;
    }
};

static BtServerCallbacks bt_server_callbacks;
static BtSecurityCallbacks bt_security_callbacks;
static BLESecurity bt_security;

static void btStartAdvertising() {
    if (!bt_initialized || !config_bt_enabled || bt_connected) return;
    BLEDevice::startAdvertising();
    bt_advertising = true;
    bt_adv_started_at = millis();
    bt_state = BT_STATE_ADVERTISING;
    btSetStatusValue("advertising");
    SERIAL_LOGLN("BT: advertising");
}

void btInit() {
    if (!config_bt_enabled || bt_initialized) {
        bt_state = config_bt_enabled ? bt_state : BT_STATE_OFF;
        return;
    }

    BLEDevice::init(config_bt_name);
    BLEDevice::setSecurityCallbacks(&bt_security_callbacks);

    if (btPasskeyConfigured()) {
        bt_security.setStaticPIN(config_bt_passkey);
        bt_security.setCapability(ESP_IO_CAP_OUT);
        bt_security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
    } else {
        bt_security.setCapability(ESP_IO_CAP_NONE);
        bt_security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    }
    bt_security.setKeySize(16);
    bt_security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    bt_security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    bt_server = BLEDevice::createServer();
    if (!bt_server) {
        bt_state = BT_STATE_ERROR;
        SERIAL_LOGLN("BT: createServer failed");
        return;
    }
    bt_server->setCallbacks(&bt_server_callbacks);

    bt_service = bt_server->createService(BT_SERVICE_UUID);
    if (!bt_service) {
        bt_state = BT_STATE_ERROR;
        SERIAL_LOGLN("BT: createService failed");
        return;
    }

    bt_status_char = bt_service->createCharacteristic(BT_STATUS_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
    if (!bt_status_char) {
        bt_state = BT_STATE_ERROR;
        SERIAL_LOGLN("BT: createCharacteristic failed");
        return;
    }
    bt_status_char->setAccessPermissions(btReadPerm());
    btSetStatusValue("idle");
    bt_service->start();

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(bt_service->getUUID());
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMaxPreferred(0x12);

    bt_initialized = true;
    bt_connected = false;
    bt_advertising = false;
    bt_bonded = false;
    bt_peer_addr[0] = '\0';

    btStartAdvertising();
    SERIAL_LOGF("BT: bare mode ready name=%s%s\n",
                  config_bt_name,
                  btPasskeyConfigured() ? " (secure pin)" : "");
}

void btShutdown() {
    if (bt_connected && bt_server) {
        bt_server->disconnect(bt_server->getConnId());
    }
    if (bt_initialized) BLEDevice::stopAdvertising();
    bt_connected = false;
    bt_advertising = false;
    bt_adv_started_at = 0;
    bt_state = BT_STATE_OFF;
    btSetStatusValue("off");
}

bool btSetEnabled(bool enabled) {
    if (!enabled) {
        btShutdown();
        config_bt_enabled = false;
        bt_bonded = false;
        bt_peer_addr[0] = '\0';
        render_requested = true;
        term_render_requested = true;
        SERIAL_LOGLN("BT: disabled");
        return false;
    }

    config_bt_enabled = true;
    if (!bt_initialized) {
        btInit();
    } else {
        btStartAdvertising();
    }
    render_requested = true;
    term_render_requested = true;
    return true;
}

void btPoll() {
    if (!config_bt_enabled) return;
    if (!bt_initialized) {
        btInit();
        return;
    }

    if (bt_advertising && !bt_connected) {
        unsigned long now = millis();
        if (now - bt_adv_started_at >= BT_ADV_TIMEOUT_MS) {
            BLEDevice::stopAdvertising();
            bt_advertising = false;
            bt_state = BT_STATE_OFF;
            btSetStatusValue("idle");
            render_requested = true;
            term_render_requested = true;
            SERIAL_LOGLN("BT: advertising timeout -> idle");
        }
    }
}
