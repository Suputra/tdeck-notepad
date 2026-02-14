#pragma once

#include <Arduino.h>
#include <ctype.h>
#include <sys/time.h>
#include <time.h>

enum TimeSyncSource {
    TIME_SYNC_NONE = 0,
    TIME_SYNC_NTP,
    TIME_SYNC_GNSS
};

static TimeSyncSource time_sync_source = TIME_SYNC_NONE;
static time_t time_sync_epoch = 0;
static uint32_t time_sync_millis = 0;
static uint32_t time_sync_ntp_count = 0;
static uint32_t time_sync_gnss_count = 0;
static uint32_t time_sync_last_gnss_try_ms = 0;
static constexpr uint32_t TIME_SYNC_GNSS_MIN_INTERVAL_MS = 30000;
static char time_sync_tz[64] = "UTC0";

const char* timeSyncSourceName(TimeSyncSource src) {
    switch (src) {
        case TIME_SYNC_NTP:  return "ntp";
        case TIME_SYNC_GNSS: return "gnss";
        default:             return "none";
    }
}

bool timeSyncSetTimeZone(const char* tz) {
    if (!tz || tz[0] == '\0') return false;
    strncpy(time_sync_tz, tz, sizeof(time_sync_tz) - 1);
    time_sync_tz[sizeof(time_sync_tz) - 1] = '\0';
    setenv("TZ", time_sync_tz, 1);
    tzset();
    return true;
}

const char* timeSyncGetTimeZone() {
    return time_sync_tz;
}

void timeSyncInit() {
    timeSyncSetTimeZone(time_sync_tz);
}

void timeSyncRecord(TimeSyncSource src) {
    time_sync_source = src;
    time_sync_epoch = time(NULL);
    time_sync_millis = millis();
    if (src == TIME_SYNC_NTP) time_sync_ntp_count++;
    if (src == TIME_SYNC_GNSS) time_sync_gnss_count++;
}

bool timeSyncClockLooksValid() {
    // 2024-01-01 00:00:00 UTC
    static constexpr time_t kMinReasonableEpoch = 1704067200;
    return time(NULL) >= kMinReasonableEpoch;
}

time_t timeSyncUtcToEpoch(struct tm* utc_tm) {
    if (!utc_tm) return (time_t)-1;

    char saved_tz[64];
    strncpy(saved_tz, time_sync_tz, sizeof(saved_tz) - 1);
    saved_tz[sizeof(saved_tz) - 1] = '\0';

    setenv("TZ", "UTC0", 1);
    tzset();
    time_t epoch = mktime(utc_tm);

    setenv("TZ", saved_tz, 1);
    tzset();
    return epoch;
}

bool timeSyncSetFromUtcFields(const char* utc_time, const char* utc_date) {
    if (!utc_time || !utc_date) return false;
    if (strlen(utc_time) < 6 || strlen(utc_date) < 6) return false;
    for (int i = 0; i < 6; i++) {
        if (!isdigit((unsigned char)utc_time[i])) return false;
        if (!isdigit((unsigned char)utc_date[i])) return false;
    }

    int hh = (utc_time[0] - '0') * 10 + (utc_time[1] - '0');
    int mm = (utc_time[2] - '0') * 10 + (utc_time[3] - '0');
    int ss = (utc_time[4] - '0') * 10 + (utc_time[5] - '0');

    int dd = (utc_date[0] - '0') * 10 + (utc_date[1] - '0');
    int mo = (utc_date[2] - '0') * 10 + (utc_date[3] - '0');
    int yy = (utc_date[4] - '0') * 10 + (utc_date[5] - '0');
    int yyyy = (yy >= 80) ? (1900 + yy) : (2000 + yy);

    if (hh < 0 || hh > 23) return false;
    if (mm < 0 || mm > 59) return false;
    if (ss < 0 || ss > 60) return false;
    if (mo < 1 || mo > 12) return false;
    if (dd < 1 || dd > 31) return false;

    struct tm tm_utc = {};
    tm_utc.tm_year = yyyy - 1900;
    tm_utc.tm_mon = mo - 1;
    tm_utc.tm_mday = dd;
    tm_utc.tm_hour = hh;
    tm_utc.tm_min = mm;
    tm_utc.tm_sec = ss;
    tm_utc.tm_isdst = 0;

    time_t epoch = timeSyncUtcToEpoch(&tm_utc);
    if (epoch < 0) return false;

    struct timeval tv = {};
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) != 0) return false;
    return true;
}

bool timeSyncMaybeFromGnss(const char* utc_time, const char* utc_date, bool has_fix) {
    if (!has_fix) return false;
    if (!utc_time || !utc_date || utc_time[0] == '\0' || utc_date[0] == '\0') return false;

    uint32_t now_ms = millis();
    if (time_sync_last_gnss_try_ms != 0 && (now_ms - time_sync_last_gnss_try_ms) < TIME_SYNC_GNSS_MIN_INTERVAL_MS) {
        return false;
    }
    time_sync_last_gnss_try_ms = now_ms;

    if (!timeSyncSetFromUtcFields(utc_time, utc_date)) return false;
    timeSyncRecord(TIME_SYNC_GNSS);
    return true;
}

void timeSyncMarkNtp() {
    timeSyncRecord(TIME_SYNC_NTP);
}

bool timeSyncGetLocalTm(struct tm* out_tm) {
    if (!out_tm) return false;
    time_t now = time(NULL);
    if (now <= 0 || !timeSyncClockLooksValid()) return false;
    localtime_r(&now, out_tm);
    return true;
}

bool timeSyncFormatLocal(char* out, size_t out_len, const char* fmt) {
    if (!out || out_len < 2 || !fmt || fmt[0] == '\0') return false;
    struct tm tm_local = {};
    if (!timeSyncGetLocalTm(&tm_local)) return false;
    return strftime(out, out_len, fmt, &tm_local) > 0;
}

bool timeSyncFormatLocalDate(char* out, size_t out_len) {
    return timeSyncFormatLocal(out, out_len, "%Y-%m-%d");
}

bool timeSyncMakeDailyFilename(char* out, size_t out_len) {
    if (!out || out_len < 14) return false; // YYYY-MM-DD.md + NUL
    char date[11];
    if (!timeSyncFormatLocalDate(date, sizeof(date))) return false;
    int n = snprintf(out, out_len, "%s.md", date);
    return n > 0 && (size_t)n < out_len;
}
