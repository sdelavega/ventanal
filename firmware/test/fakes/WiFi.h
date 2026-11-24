// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "Arduino.h"
#include <algorithm>
#include <array>
#include <vector>
constexpr int WIFI_OFF=0, WIFI_STA = 1, WL_CONNECTED = 3;
constexpr int WIFI_SCAN_RUNNING = -1, WIFI_SCAN_FAILED = -2;
enum wifi_scan_method_t { WIFI_ALL_CHANNEL_SCAN };
enum wifi_sort_method_t { WIFI_CONNECT_AP_BY_SIGNAL };
enum wifi_ps_type_t { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM };
struct FakeWiFi {
    struct AP { int rssi; int channel; std::array<uint8_t, 6> bssid; };
    std::vector<AP> aps;
    bool connected = true, failPinned = false;
    int currentRssi = -50, currentChannel = 1;
    int pinnedBegins = 0, plainBegins = 0, disconnects = 0;
    int selectedChannel = 0;
    std::array<uint8_t, 6> selectedBssid{};
    wifi_ps_type_t sleeping = WIFI_PS_MAX_MODEM;
    unsigned sleepChanges = 0;
    wifi_ps_type_t getSleep() { return sleeping; }
    bool setSleep(wifi_ps_type_t value) { sleeping=value; ++sleepChanges; return true; }
    bool setSleep(bool value) { return setSleep(value ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE); }
    bool disconnect(bool) { ++disconnects; connected = false; return true; }
    void persistent(bool) {}
    void mode(int) {}
    void setScanMethod(wifi_scan_method_t) {}
    void setSortMethod(wifi_sort_method_t) {}
    void begin(const char*, const char*) { ++plainBegins; connected = true; }
    void begin(const char*, const char*, int channel, const uint8_t* bssid) {
        ++pinnedBegins; selectedChannel = channel;
        std::copy_n(bssid, 6, selectedBssid.begin());
        connected = !failPinned;
        if (connected) for (const auto& ap : aps) if (ap.bssid == selectedBssid) { currentRssi = ap.rssi; currentChannel = ap.channel; }
    }
    int RSSI() { return currentRssi; }
    int RSSI(int i) { return aps.at(i).rssi; }
    int channel() { return currentChannel; }
    int channel(int i) { return aps.at(i).channel; }
    uint8_t* BSSID(int i) { return aps.at(i).bssid.data(); }
    int status() { return connected ? WL_CONNECTED : 0; }
    int scanNetworks(bool, bool, bool, uint32_t, uint8_t, const char*) { return int(aps.size()); }
    int scanComplete() { return WIFI_SCAN_FAILED; }
    void scanDelete() {}
};
inline FakeWiFi WiFi;
// Records the configured POSIX TZ string so host tests can prove the
// timezone is initialised in setup, independent of connectivity.
inline std::string fakeTz;
inline void configTzTime(const char* tz, const char*, const char*) { fakeTz = tz; }
