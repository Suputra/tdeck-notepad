#pragma once

#include <Arduino.h>
#include <ctype.h>

struct GnssSnapshot {
    bool power_on;
    bool has_fix;
    bool has_time;
    bool has_location;
    bool has_hdop;
    bool has_altitude;
    bool has_rmc;
    bool has_gga;
    int satellites;
    float hdop;
    float altitude_m;
    double latitude_deg;
    double longitude_deg;
    char utc_time[16];
    char utc_date[8];
    char last_sentence[96];
    char last_rmc[96];
    char last_gga[96];
    uint32_t total_bytes;
    uint32_t total_sentences;
    uint32_t checksum_failures;
    uint32_t parse_failures;
    uint32_t last_rx_ms;
};

static GnssSnapshot gnss_state = {};
static bool gnss_rmc_fix = false;
static bool gnss_gga_fix = false;
static bool gnss_uart_started = false;
static char gnss_line_buf[128];
static size_t gnss_line_len = 0;

int gnssHexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

void gnssCopyStr(char* dst, size_t dst_len, const char* src) {
    if (!dst || dst_len < 1) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

bool gnssParseInt(const char* s, int* out) {
    if (!s || !*s || !out) return false;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || end == s) return false;
    *out = (int)v;
    return true;
}

bool gnssParseFloat(const char* s, float* out) {
    if (!s || !*s || !out) return false;
    char* end = NULL;
    float v = strtof(s, &end);
    if (!end || end == s) return false;
    *out = v;
    return true;
}

bool gnssParseCoord(const char* raw, char hemi, double* out_deg) {
    if (!raw || !*raw || !out_deg) return false;
    char* end = NULL;
    double packed = strtod(raw, &end);
    if (!end || end == raw) return false;

    int degrees = (int)(packed / 100.0);
    double minutes = packed - (double)(degrees * 100);
    double deg = (double)degrees + (minutes / 60.0);

    if (hemi == 'S' || hemi == 'W') deg = -deg;
    else if (hemi != 'N' && hemi != 'E') return false;

    *out_deg = deg;
    return true;
}

bool gnssChecksumOk(const char* sentence) {
    if (!sentence || sentence[0] != '$') return false;
    const char* star = strchr(sentence, '*');
    if (!star || star[1] == '\0' || star[2] == '\0') return false;

    uint8_t sum = 0;
    for (const char* p = sentence + 1; p < star; p++) {
        sum ^= (uint8_t)(*p);
    }

    int hi = gnssHexNibble(star[1]);
    int lo = gnssHexNibble(star[2]);
    if (hi < 0 || lo < 0) return false;
    return sum == (uint8_t)((hi << 4) | lo);
}

int gnssSplitCsv(char* s, char** fields, int max_fields) {
    if (!s || !fields || max_fields < 1) return 0;
    int count = 0;
    fields[count++] = s;

    while (*s && count < max_fields) {
        if (*s == ',') {
            *s = '\0';
            fields[count++] = s + 1;
        } else if (*s == '*') {
            *s = '\0';
            break;
        }
        s++;
    }
    return count;
}

void gnssUpdateFixFlag() {
    gnss_state.has_fix = gnss_rmc_fix || gnss_gga_fix;
}

void gnssParseRmc(char** fields, int n, const char* sentence) {
    gnss_state.has_rmc = true;
    gnssCopyStr(gnss_state.last_rmc, sizeof(gnss_state.last_rmc), sentence);

    if (n > 1 && fields[1][0] != '\0') {
        gnssCopyStr(gnss_state.utc_time, sizeof(gnss_state.utc_time), fields[1]);
    }
    if (n > 9 && fields[9][0] != '\0') {
        gnssCopyStr(gnss_state.utc_date, sizeof(gnss_state.utc_date), fields[9]);
    }
    gnss_state.has_time = (gnss_state.utc_time[0] != '\0' && gnss_state.utc_date[0] != '\0');

    char status = (n > 2 && fields[2][0] != '\0') ? fields[2][0] : 'V';
    gnss_rmc_fix = (status == 'A');
    gnssUpdateFixFlag();

    if (n > 6) {
        double lat = 0.0;
        double lon = 0.0;
        char lat_hemi = fields[4][0];
        char lon_hemi = fields[6][0];
        if (gnssParseCoord(fields[3], lat_hemi, &lat) && gnssParseCoord(fields[5], lon_hemi, &lon)) {
            gnss_state.latitude_deg = lat;
            gnss_state.longitude_deg = lon;
            gnss_state.has_location = true;
        }
    }
}

void gnssParseGga(char** fields, int n, const char* sentence) {
    gnss_state.has_gga = true;
    gnssCopyStr(gnss_state.last_gga, sizeof(gnss_state.last_gga), sentence);

    if (n > 1 && fields[1][0] != '\0') {
        gnssCopyStr(gnss_state.utc_time, sizeof(gnss_state.utc_time), fields[1]);
    }

    if (n > 6) {
        int fix_quality = 0;
        if (gnssParseInt(fields[6], &fix_quality)) {
            gnss_gga_fix = (fix_quality > 0);
        } else {
            gnss_gga_fix = false;
        }
        gnssUpdateFixFlag();
    }

    if (n > 7) {
        int sats = 0;
        if (gnssParseInt(fields[7], &sats)) {
            gnss_state.satellites = sats;
        }
    }

    if (n > 8) {
        float hdop = 0.0f;
        if (gnssParseFloat(fields[8], &hdop)) {
            gnss_state.hdop = hdop;
            gnss_state.has_hdop = true;
        }
    }

    if (n > 9) {
        float alt = 0.0f;
        if (gnssParseFloat(fields[9], &alt)) {
            gnss_state.altitude_m = alt;
            gnss_state.has_altitude = true;
        }
    }

    if (n > 5) {
        double lat = 0.0;
        double lon = 0.0;
        char lat_hemi = fields[3][0];
        char lon_hemi = fields[5][0];
        if (gnssParseCoord(fields[2], lat_hemi, &lat) && gnssParseCoord(fields[4], lon_hemi, &lon)) {
            gnss_state.latitude_deg = lat;
            gnss_state.longitude_deg = lon;
            gnss_state.has_location = true;
        }
    }
}

void gnssHandleSentence(const char* sentence) {
    if (!sentence || sentence[0] != '$') return;

    gnss_state.total_sentences++;
    gnssCopyStr(gnss_state.last_sentence, sizeof(gnss_state.last_sentence), sentence);

    if (!gnssChecksumOk(sentence)) {
        gnss_state.checksum_failures++;
        return;
    }

    char work[128];
    gnssCopyStr(work, sizeof(work), sentence + 1); // skip '$'

    char* fields[20];
    int n = gnssSplitCsv(work, fields, 20);
    if (n < 1 || strlen(fields[0]) < 5) {
        gnss_state.parse_failures++;
        return;
    }

    const size_t type_len = strlen(fields[0]);
    const char* kind = fields[0] + (type_len - 3);

    if (strcmp(kind, "RMC") == 0) {
        gnssParseRmc(fields, n, sentence);
    } else if (strcmp(kind, "GGA") == 0) {
        gnssParseGga(fields, n, sentence);
    }

    if (timeSyncMaybeFromGnss(gnss_state.utc_time, gnss_state.utc_date, gnss_state.has_fix)) {
        Serial.println("Clock: synced from GNSS");
    }

    gnss_state.last_rx_ms = millis();
}

void gnssResetSessionData() {
    gnss_rmc_fix = false;
    gnss_gga_fix = false;
    gnss_state.has_fix = false;
    gnss_state.has_time = false;
    gnss_state.has_location = false;
    gnss_state.has_hdop = false;
    gnss_state.has_altitude = false;
    gnss_state.has_rmc = false;
    gnss_state.has_gga = false;
    gnss_state.satellites = -1;
    gnss_state.hdop = 0.0f;
    gnss_state.altitude_m = 0.0f;
    gnss_state.latitude_deg = 0.0;
    gnss_state.longitude_deg = 0.0;
    gnss_state.utc_time[0] = '\0';
    gnss_state.utc_date[0] = '\0';
    gnss_state.last_sentence[0] = '\0';
    gnss_state.last_rmc[0] = '\0';
    gnss_state.last_gga[0] = '\0';
    gnss_state.total_bytes = 0;
    gnss_state.total_sentences = 0;
    gnss_state.checksum_failures = 0;
    gnss_state.parse_failures = 0;
    gnss_state.last_rx_ms = 0;
    gnss_line_len = 0;
}

void gnssInit() {
    memset(&gnss_state, 0, sizeof(gnss_state));
    gnss_state.satellites = -1;
    gnss_state.power_on = false;
    gnss_rmc_fix = false;
    gnss_gga_fix = false;
    gnss_uart_started = false;
    gnss_line_len = 0;
    pinMode(BOARD_1V8_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, LOW);
    pinMode(BOARD_GPS_EN, OUTPUT);
    digitalWrite(BOARD_GPS_EN, LOW);
}

bool gnssSetPower(bool on) {
    if (on) {
        if (gnss_state.power_on) return false;
        digitalWrite(BOARD_1V8_EN, HIGH);
        delay(5);
        digitalWrite(BOARD_GPS_EN, HIGH);
        delay(20);
        Serial2.begin(9600, SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
        gnss_uart_started = true;
        gnss_state.power_on = true;
        gnssResetSessionData();
        gnss_state.power_on = true;
        Serial.println("GNSS: powered on");
        return true;
    }

    if (!gnss_state.power_on) return false;
    if (gnss_uart_started) {
        Serial2.end();
        gnss_uart_started = false;
    }
    digitalWrite(BOARD_GPS_EN, LOW);
    digitalWrite(BOARD_1V8_EN, LOW);
    gnss_state.power_on = false;
    gnss_rmc_fix = false;
    gnss_gga_fix = false;
    gnssUpdateFixFlag();
    gnss_line_len = 0;
    Serial.println("GNSS: powered off");
    return true;
}

bool gnssIsPowered() {
    return gnss_state.power_on;
}

void gnssGetSnapshot(GnssSnapshot* out) {
    if (!out) return;
    *out = gnss_state;
}

void gnssPoll() {
    if (!gnss_state.power_on || !gnss_uart_started) return;

    int budget = 512;
    while (budget-- > 0 && Serial2.available() > 0) {
        int b = Serial2.read();
        if (b < 0) break;

        char c = (char)b;
        gnss_state.total_bytes++;

        if (c == '\r') continue;
        if (c == '\n') {
            if (gnss_line_len > 0) {
                gnss_line_buf[gnss_line_len] = '\0';
                gnssHandleSentence(gnss_line_buf);
                gnss_line_len = 0;
            }
            continue;
        }

        if (gnss_line_len == 0 && c != '$') continue;

        if (gnss_line_len < sizeof(gnss_line_buf) - 1) {
            gnss_line_buf[gnss_line_len++] = c;
        } else {
            gnss_state.parse_failures++;
            gnss_line_len = 0;
        }
    }
}

bool gnssFormatUtc(const GnssSnapshot* snap, char* out, size_t out_len) {
    if (!snap || !out || out_len < 2) return false;
    if (strlen(snap->utc_time) < 6 || strlen(snap->utc_date) < 6) return false;

    const char* t = snap->utc_time;
    const char* d = snap->utc_date;
    for (int i = 0; i < 6; i++) {
        if (!isdigit((unsigned char)t[i]) || !isdigit((unsigned char)d[i])) return false;
    }

    int hh = (t[0] - '0') * 10 + (t[1] - '0');
    int mm = (t[2] - '0') * 10 + (t[3] - '0');
    int ss = (t[4] - '0') * 10 + (t[5] - '0');

    int dd = (d[0] - '0') * 10 + (d[1] - '0');
    int mo = (d[2] - '0') * 10 + (d[3] - '0');
    int yy = (d[4] - '0') * 10 + (d[5] - '0');
    int yyyy = (yy >= 80) ? (1900 + yy) : (2000 + yy);

    snprintf(out, out_len, "%04d-%02d-%02d %02d:%02d:%02dZ", yyyy, mo, dd, hh, mm, ss);
    return true;
}
