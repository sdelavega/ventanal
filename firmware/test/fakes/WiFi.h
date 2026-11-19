// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "Arduino.h"
constexpr int WIFI_OFF=0, WIFI_STA = 1, WL_CONNECTED = 3;
constexpr int WIFI_SCAN_RUNNING = -1, WIFI_SCAN_FAILED = -2;
enum wifi_scan_method_t { WIFI_ALL_CHANNEL_SCAN };
enum wifi_sort_method_t { WIFI_CONNECT_AP_BY_SIGNAL };
enum wifi_ps_type_t { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM };
struct FakeWiFi {
    wifi_ps_type_t sleeping = WIFI_PS_MAX_MODEM;
    unsigned sleepChanges = 0;
    wifi_ps_type_t getSleep() { return sleeping; }
    bool setSleep(wifi_ps_type_t value) { sleeping=value; ++sleepChanges; return true; }
    bool setSleep(bool value) { return setSleep(value ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE); }
    bool disconnect(bool) { return true; }
    void persistent(bool) {}
    void mode(int) {}
    void setScanMethod(wifi_scan_method_t) {}
    void setSortMethod(wifi_sort_method_t) {}
    void begin(const char*, const char*) {}
    int RSSI() { return -50; }
    int RSSI(int) { return 0; }
    int status() { return WL_CONNECTED; }
    int scanNetworks(bool, bool, bool, uint32_t, uint8_t, const char*) { return WIFI_SCAN_FAILED; }
    int scanComplete() { return WIFI_SCAN_FAILED; }
    void scanDelete() {}
};
inline FakeWiFi WiFi;
// Records the configured POSIX TZ string so host tests can prove the
// timezone is initialised in setup, independent of connectivity.
inline std::string fakeTz;
inline void configTzTime(const char* tz, const char*, const char*) { fakeTz = tz; }
