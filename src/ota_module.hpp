#pragma once

#include <ArduinoOTA.h>

// Network push OTA (ArduinoOTA over WiFi + mDNS).
//
// The device advertises itself on the LAN as "<host>.local". Push a new build
// with PlatformIO's espota uploader (see the [env:T-Deck-Pro-ota] env), or with
// the Arduino IDE network-port picker.
//
// Init is lazy: ArduinoOTA.begin() needs WiFi up, which only happens on demand
// here, so otaHandle() brings the listener up the first time the link is
// connected and drives it every loop after that.

static bool ota_initialized = false;

void otaSetup() {
    if (ota_initialized) return;

    ArduinoOTA.setHostname(config_ota_host[0] ? config_ota_host : "s-term");
    if (config_ota_pass[0] != '\0') {
        ArduinoOTA.setPassword(config_ota_pass);
    }

    ArduinoOTA.onStart([]() {
        const char* type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
        SERIAL_LOGF("OTA: start (%s)\n", type);
        connectMsg("OTA: updating %s...", type);
    });
    ArduinoOTA.onEnd([]() {
        SERIAL_LOGLN("OTA: done, rebooting");
        connectMsg("OTA: done, rebooting");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        // Serial only — e-ink refreshes are too slow for per-chunk progress.
        SERIAL_LOGF("OTA: %u%%\r", total ? (progress * 100U / total) : 0U);
    });
    ArduinoOTA.onError([](ota_error_t error) {
        const char* reason = "unknown";
        switch (error) {
            case OTA_AUTH_ERROR:    reason = "auth failed";    break;
            case OTA_BEGIN_ERROR:   reason = "begin failed";   break;
            case OTA_CONNECT_ERROR: reason = "connect failed"; break;
            case OTA_RECEIVE_ERROR: reason = "receive failed"; break;
            case OTA_END_ERROR:     reason = "end failed";     break;
        }
        SERIAL_LOGF("OTA: error %u (%s)\n", error, reason);
        connectMsg("OTA: error (%s)", reason);
    });

    ArduinoOTA.begin();
    ota_initialized = true;
    SERIAL_LOGF("OTA: ready as '%s.local'%s\n",
                config_ota_host[0] ? config_ota_host : "s-term",
                config_ota_pass[0] ? "" : " (no password)");
}

void otaHandle() {
    if (!hasNetwork()) return;
    if (!ota_initialized) otaSetup();
    ArduinoOTA.handle();
}
