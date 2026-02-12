// --- Bluetooth LE HID Keyboard (bonding + keepalive) ---

#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <BLEUtils.h>
#include <HIDTypes.h>
#include <esp_gap_ble_api.h>
#include <cstring>

#define BT_PASSKEY_MIN            100000U
#define BT_PASSKEY_MAX            999999U
#define BT_ADV_RESTART_MS         250U
#define BT_KEEPALIVE_INTERVAL_MS  20000U
#define BT_KEEPALIVE_DELAY_MS     700U

#define BT_HID_MOD_LCTRL          0x01
#define BT_HID_MOD_LSHIFT         0x02
#define BT_HID_MOD_LALT           0x04
#define BT_HID_MOD_LGUI           0x08

#define BT_HID_KEY_ENTER          0x28
#define BT_HID_KEY_TAB            0x2B
#define BT_HID_KEY_BACKSPACE      0x2A
#define BT_HID_KEY_UP             0x52
#define BT_HID_KEY_DOWN           0x51
#define BT_HID_KEY_LEFT           0x50
#define BT_HID_KEY_RIGHT          0x4F
#define BT_HID_KEY_F24            0x73

enum BtState { BT_STATE_OFF, BT_STATE_ADVERTISING, BT_STATE_CONNECTED, BT_STATE_ERROR };

static volatile BtState bt_state = BT_STATE_OFF;
static volatile bool bt_initialized = false;
static volatile bool bt_connected = false;
static volatile bool bt_advertising = false;
static volatile bool bt_bonded = false;
static volatile bool bt_adv_restart_pending = false;
static volatile bool bt_keepalive_pending = false;
static unsigned long bt_adv_restart_at = 0;
static unsigned long bt_connected_at = 0;
static unsigned long bt_last_report_at = 0;

static BLEServer* bt_server = NULL;
static BLEHIDDevice* bt_hid = NULL;
static BLECharacteristic* bt_input_keyboard = NULL;
static BLECharacteristic* bt_output_keyboard = NULL;
static BLECharacteristic* bt_boot_input = NULL;

static char bt_peer_addr[18] = "";

// ASCII -> HID usage (bit7 = requires left shift)
static const uint8_t bt_ascii_map[128] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x2A,0x2B,0x28,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x2C,0x9E,0xB4,0xA0,0xA1,0xA2,0xA4,0x34,0xA6,0xA7,0xA5,0xAE,0x36,0x2D,0x37,0x38,
    0x27,0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0xB3,0x33,0xB6,0x2E,0xB7,0xB8,
    0x9F,0x84,0x85,0x86,0x87,0x88,0x89,0x8A,0x8B,0x8C,0x8D,0x8E,0x8F,0x90,0x91,0x92,
    0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0x9B,0x9C,0x9D,0x2F,0x31,0x30,0xA3,0xAD,
    0x35,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,
    0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0xAF,0xB1,0xB0,0xB5,0x00
};

static uint8_t bt_hid_report_map[] = {
    USAGE_PAGE(1),      0x01,       // Generic Desktop
    USAGE(1),           0x06,       // Keyboard
    COLLECTION(1),      0x01,       // Application
    REPORT_ID(1),       0x01,
    USAGE_PAGE(1),      0x07,       // Key codes
    USAGE_MINIMUM(1),   0xE0,
    USAGE_MAXIMUM(1),   0xE7,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x01,
    REPORT_SIZE(1),     0x01,
    REPORT_COUNT(1),    0x08,
    HIDINPUT(1),        0x02,
    REPORT_COUNT(1),    0x01,
    REPORT_SIZE(1),     0x08,
    HIDINPUT(1),        0x01,
    REPORT_COUNT(1),    0x05,
    REPORT_SIZE(1),     0x01,
    USAGE_PAGE(1),      0x08,       // LEDs
    USAGE_MINIMUM(1),   0x01,
    USAGE_MAXIMUM(1),   0x05,
    HIDOUTPUT(1),       0x02,
    REPORT_COUNT(1),    0x01,
    REPORT_SIZE(1),     0x03,
    HIDOUTPUT(1),       0x01,
    REPORT_COUNT(1),    0x06,
    REPORT_SIZE(1),     0x08,
    LOGICAL_MINIMUM(1), 0x00,
    LOGICAL_MAXIMUM(1), 0x73,       // F24
    USAGE_PAGE(1),      0x07,
    USAGE_MINIMUM(1),   0x00,
    USAGE_MAXIMUM(1),   0x73,
    HIDINPUT(1),        0x00,
    END_COLLECTION(0)
};

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

static esp_gatt_perm_t btWritePerm() {
    return btPasskeyConfigured() ? ESP_GATT_PERM_WRITE_ENC_MITM : ESP_GATT_PERM_WRITE_ENCRYPTED;
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

static bool btSendReport(uint8_t modifiers, uint8_t usage0) {
    if (!bt_initialized || !bt_connected || !bt_input_keyboard) return false;

    uint8_t report[8] = {modifiers, 0x00, usage0, 0x00, 0x00, 0x00, 0x00, 0x00};
    bt_input_keyboard->setValue(report, sizeof(report));
    bt_input_keyboard->notify();

    if (bt_boot_input) {
        bt_boot_input->setValue(report, sizeof(report));
        bt_boot_input->notify();
    }

    bt_last_report_at = millis();
    return true;
}

bool btSendUsage(uint8_t usage, uint8_t modifiers) {
    if (!btSendReport(modifiers, usage)) return false;
    delay(6);
    return btSendReport(0x00, 0x00);
}

static bool btAsciiToHid(char c, uint8_t* usage, uint8_t* modifiers) {
    if (!usage || !modifiers) return false;
    uint8_t uc = (uint8_t)c;
    if (uc >= 128) return false;
    uint8_t m = bt_ascii_map[uc];
    if (m == 0x00) return false;

    *usage = (m & 0x7F);
    *modifiers = (m & 0x80) ? BT_HID_MOD_LSHIFT : 0;
    return true;
}

bool btTypeChar(char c, uint8_t extra_modifiers) {
    uint8_t usage = 0;
    uint8_t modifiers = 0;
    if (!btAsciiToHid(c, &usage, &modifiers)) return false;
    return btSendUsage(usage, modifiers | extra_modifiers);
}

size_t btTypeTextN(const char* text, size_t len, uint8_t extra_modifiers) {
    if (!text || len == 0) return 0;

    size_t sent = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        bool ok = false;

        if (c == '\r') {
            continue;
        } else if (c == '\n') {
            ok = btSendUsage(BT_HID_KEY_ENTER, extra_modifiers);
        } else if (c == '\t') {
            ok = btSendUsage(BT_HID_KEY_TAB, extra_modifiers);
        } else if (c == '\b') {
            ok = btSendUsage(BT_HID_KEY_BACKSPACE, extra_modifiers);
        } else {
            ok = btTypeChar(c, extra_modifiers);
        }

        if (!ok) break;
        sent++;
        delay(4);
    }
    return sent;
}

size_t btTypeText(const char* text) {
    if (!text) return 0;
    return btTypeTextN(text, strlen(text), 0);
}

static bool btSendKeepaliveKeypress() {
    // F24 is typically ignored by phones and works as a low-impact keepalive tap.
    return btSendUsage(BT_HID_KEY_F24, 0);
}

class BtServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        (void)pServer;
        bt_connected = true;
        bt_advertising = false;
        bt_state = BT_STATE_CONNECTED;
        bt_connected_at = millis();
        bt_last_report_at = bt_connected_at;
        bt_keepalive_pending = true;
        render_requested = true;
        term_render_requested = true;
        Serial.println("BT: client connected");
    }

    void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
        onConnect(pServer);
        if (param) {
            btFormatAddr(param->connect.remote_bda, bt_peer_addr, sizeof(bt_peer_addr));
            Serial.printf("BT: peer=%s\n", bt_peer_addr);
            esp_ble_sec_act_t sec_act = btPasskeyConfigured() ? ESP_BLE_SEC_ENCRYPT_MITM : ESP_BLE_SEC_ENCRYPT_NO_MITM;
            esp_err_t err = esp_ble_set_encryption(param->connect.remote_bda, sec_act);
            if (err != ESP_OK) {
                Serial.printf("BT: set_encryption failed err=%d\n", (int)err);
            }
        }
    }

    void onDisconnect(BLEServer* pServer) override {
        (void)pServer;
        bt_connected = false;
        bt_advertising = false;
        bt_state = config_bt_enabled ? BT_STATE_ADVERTISING : BT_STATE_OFF;
        bt_adv_restart_pending = config_bt_enabled;
        bt_adv_restart_at = millis() + BT_ADV_RESTART_MS;
        bt_keepalive_pending = true;
        render_requested = true;
        term_render_requested = true;
        Serial.println("BT: client disconnected");
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
        Serial.printf("BT: passkey notify %06u\n", (unsigned)pass_key);
    }

    bool onSecurityRequest() override {
        return true;
    }

    void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) override {
        bt_bonded = auth_cmpl.success;
        btFormatAddr(auth_cmpl.bd_addr, bt_peer_addr, sizeof(bt_peer_addr));
        if (auth_cmpl.success) {
            Serial.printf("BT: paired with %s\n", bt_peer_addr);
        } else {
            Serial.printf("BT: pair failed reason=0x%02X\n", auth_cmpl.fail_reason);
        }
        render_requested = true;
        term_render_requested = true;
    }

    bool onConfirmPIN(uint32_t pin) override {
        Serial.printf("BT: confirm pin %06u\n", (unsigned)pin);
        return true;
    }
};

static BtServerCallbacks bt_server_callbacks;
static BtSecurityCallbacks bt_security_callbacks;
static BLESecurity bt_security;

static void btStartAdvertising() {
    if (!bt_initialized || !config_bt_enabled) return;
    if (bt_connected) return;
    BLEDevice::startAdvertising();
    bt_advertising = true;
    bt_state = BT_STATE_ADVERTISING;
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
        Serial.println("BT: createServer failed");
        return;
    }
    bt_server->setCallbacks(&bt_server_callbacks);

    bt_hid = new BLEHIDDevice(bt_server);
    bt_input_keyboard = bt_hid->inputReport(1);
    bt_output_keyboard = bt_hid->outputReport(1);
    bt_boot_input = bt_hid->bootInput();

    if (!bt_input_keyboard || !bt_output_keyboard) {
        bt_state = BT_STATE_ERROR;
        Serial.println("BT: create HID reports failed");
        return;
    }

    bt_input_keyboard->setAccessPermissions(btReadPerm());
    bt_output_keyboard->setAccessPermissions(btWritePerm());
    if (bt_boot_input) bt_boot_input->setAccessPermissions(btReadPerm());

    bt_hid->manufacturer()->setValue("LilyGO");
    bt_hid->pnp(0x02, 0x303A, 0x1001, 0x0110);
    bt_hid->hidInfo(0x00, 0x01);
    bt_hid->reportMap(bt_hid_report_map, sizeof(bt_hid_report_map));
    bt_hid->startServices();
    bt_hid->setBatteryLevel(100);

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(bt_hid->hidService()->getUUID());
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMaxPreferred(0x12);

    bt_initialized = true;
    bt_connected = false;
    bt_bonded = false;
    bt_adv_restart_pending = false;
    bt_keepalive_pending = true;
    bt_peer_addr[0] = '\0';

    btStartAdvertising();
    Serial.printf("BT: HID ready name=%s%s\n",
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
    bt_adv_restart_pending = false;
    bt_state = BT_STATE_OFF;
}

void btPoll() {
    if (!config_bt_enabled) return;
    if (!bt_initialized) {
        btInit();
        return;
    }

    if (bt_adv_restart_pending && millis() >= bt_adv_restart_at) {
        bt_adv_restart_pending = false;
        btStartAdvertising();
    } else if (!bt_connected && !bt_advertising && !bt_adv_restart_pending) {
        btStartAdvertising();
    }

    if (!bt_connected) return;

    unsigned long now = millis();
    if (bt_keepalive_pending) {
        if (now - bt_connected_at >= BT_KEEPALIVE_DELAY_MS) {
            btSendKeepaliveKeypress();
            bt_keepalive_pending = false;
        }
        return;
    }

    if (now - bt_last_report_at >= BT_KEEPALIVE_INTERVAL_MS) {
        btSendKeepaliveKeypress();
    }
}
