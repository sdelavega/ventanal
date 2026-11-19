// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Board side of the setup portal. Compiled into a board by a one-line .cpp
// in its sketch directory that includes this file.
#include "portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <esp_wifi.h>
#include <memory>

namespace portal {
namespace {

constexpr int maxNetworks = 20;

struct Runtime {
    DNSServer dns;
    WebServer server{80};
    std::unique_ptr<PortalCore> core;
    void (*userFeed)() = nullptr;
    void (*note)(const char*) = nullptr;
    int clients = -1;
    wifi_event_id_t probeWatch = 0;
    uint32_t probeReportMs = 0;
};

// Probe requests heard by the hotspot (phones looking for networks): count
// and strongest signal per report window. Addresses are never kept.
std::atomic<uint32_t> probeCount{0};
std::atomic<int> probeRssiMax{-128};

Runtime* active = nullptr;



void serviceRequests() {
    if (!active) return;
    active->dns.processNextRequest();
    active->server.handleClient();
}

void feedAndServe() {
    if (active && active->userFeed) active->userFeed();
    serviceRequests();
}

// WebServer splits an urlencoded body into arguments; rebuild the body the
// shared parser reads.
std::string encodedBody(WebServer& server) {
    static const char hex[] = "0123456789ABCDEF";
    std::string body;
    auto append = [&](const String& text) {
        for (unsigned i = 0; i < text.length(); ++i) {
            const char c = text[i];
            const auto u = static_cast<unsigned char>(c);
            if (isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~') {
                body += c;
            } else {
                body += '%';
                body += hex[u >> 4];
                body += hex[u & 15];
            }
        }
    };
    for (int i = 0; i < server.args(); ++i) {
        const String name = server.argName(i);
        if (name == "plain") continue;
        if (!body.empty()) body += '&';
        append(name);
        body += '=';
        append(server.arg(i));
    }
    return body;
}

void handleRequest() {
    if (!active) return;
    WebServer& server = active->server;
    const std::string path = server.uri().c_str();
    const uint32_t now = millis();
    const Response r = server.method() == HTTP_POST
        ? active->core->handlePost(path, encodedBody(server).c_str(), now)
        : active->core->handleGet(path, now);
    server.sendHeader("Cache-Control", "no-store");
    if (r.code == 302) server.sendHeader("Location", r.location.c_str());
    server.send(r.code, r.type.c_str(), r.body.c_str());
}

std::vector<ScannedNetwork> scanNetworks() {
    std::vector<ScannedNetwork> found;
    const int n = WiFi.scanNetworks();
    for (int i = 0; i < n; ++i) {
        const std::string ssid = WiFi.SSID(i).c_str();
        if (ssid.empty() || ssid.size() > 32) continue;
        found.push_back({ssid, WiFi.RSSI(i), int(WiFi.channel(i))});
    }
    WiFi.scanDelete();
    return found;
}

// The page's list: unique names, strongest first.
std::vector<std::string> networkNames(std::vector<ScannedNetwork> found) {
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.rssi > b.rssi; });
    std::vector<std::string> names;
    for (const auto& f : found) {
        if (int(names.size()) == maxNetworks) break;
        if (std::find(names.begin(), names.end(), f.ssid) == names.end()) names.push_back(f.ssid);
    }
    return names;
}

}  // namespace

bool SetupPortal::begin(const char* apSsid, const char* apPass, PortalHooks hooks, const char* preferredSsid,
                        const PortalFields* current) {
    if (active || strlen(apPass) < 8) return false;
    auto runtime = std::make_unique<Runtime>();
    runtime->userFeed = hooks.feed;
    runtime->note = hooks.note;
    hooks.feed = feedAndServe;  // long checks keep answering the phone
    // The station must stay idle while the hotspot runs: background
    // reconnects scan every channel and the hotspot stops beaconing on its
    // own (phones took minutes to see it). The old network is forgotten in
    // RAM only; persistence is off, so nothing saved changes.
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, true);
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    const std::vector<ScannedNetwork> scan = scanNetworks();
    const int channel = chooseApChannel(scan, preferredSsid);
    std::vector<std::string> networks = networkNames(scan);
    const size_t scanned = networks.size();
    const IPAddress address(192, 168, 4, 1);
    if (!WiFi.softAPConfig(address, address, IPAddress(255, 255, 255, 0))
        || !WiFi.softAP(apSsid, apPass, channel)) {  // an 8+ character passphrase means WPA2-PSK
        WiFi.mode(WIFI_OFF);
        return false;
    }
    if (portalApVariant & 4) WiFi.mode(WIFI_AP);  // station off until a join needs it
    runtime->core = std::make_unique<PortalCore>(hooks, std::move(networks), millis(),
                                                 current ? *current : PortalFields{});
    runtime->dns.setErrorReplyCode(DNSReplyCode::NoError);
    runtime->dns.start(53, "*", address);  // every name resolves to the portal
    runtime->server.onNotFound(handleRequest);
    runtime->server.begin();
    active = runtime.release();
    if (portalApVariant & 1) esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    if (portalApVariant & 2) esp_wifi_set_country_code("US", true);
    {
        wifi_bandwidth_t bw = WIFI_BW_HT40;
        esp_wifi_get_bandwidth(WIFI_IF_AP, &bw);
        char cc[3] = {};
        esp_wifi_get_country_code(cc);
        portalNote("portal variant=%d bandwidth=%d country=%s", portalApVariant, int(bw), cc);
    }
    esp_wifi_set_event_mask(WIFI_EVENT_MASK_NONE);  // deliver AP probe-request events while setup runs
    active->probeWatch = WiFi.onEvent(
        [](arduino_event_id_t, arduino_event_info_t info) {
            probeCount.fetch_add(1, std::memory_order_relaxed);
            const int rssi = info.wifi_ap_probereqrecved.rssi;
            int seen = probeRssiMax.load(std::memory_order_relaxed);
            while (rssi > seen && !probeRssiMax.compare_exchange_weak(seen, rssi)) {}
        },
        ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED);
    int8_t power = 0;
    esp_wifi_get_max_tx_power(&power);
    portalNote("portal ap=up channel=%d chosen=%d networks=%u tx_quarter_dbm=%d", int(WiFi.channel()), channel,
               unsigned(scanned), int(power));
    return true;
}

PortalOutcome SetupPortal::poll(uint32_t nowMs) {
    if (!active) return PortalOutcome::Cancelled;
    serviceRequests();
    if (nowMs - active->probeReportMs >= 10000) {
        active->probeReportMs = nowMs;
        const uint32_t probes = probeCount.exchange(0);
        const int rssi = probeRssiMax.exchange(-128);
        portalNote("portal probes=%lu rssi_max=%d", static_cast<unsigned long>(probes), probes ? rssi : 0);
    }
    const int clients = WiFi.softAPgetStationNum();
    if (clients != active->clients) {
        active->clients = clients;
        portalNote("portal clients=%d sta=%d channel=%d", clients, int(WiFi.status()), int(WiFi.channel()));
    }
    return active->core->poll(nowMs);
}

void SetupPortal::cancel() {
    if (active) active->core->cancel();
}

const char* SetupPortal::status() const {
    return active ? active->core->status() : "";
}

bool SetupPortal::statusIsFailure() const {
    return active && active->core->statusIsFailure();
}

void SetupPortal::end() {
    if (!active) return;
    WiFi.removeEvent(active->probeWatch);
    esp_wifi_set_event_mask(WIFI_EVENT_MASK_AP_PROBEREQRECVED);  // the default
    active->server.stop();
    active->dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.setSleep(true);
    WiFi.setAutoReconnect(true);
    delete active;
    active = nullptr;
}

int portalApVariant = 0;

void portalNote(const char* format, ...) {
    if (!active || !active->note) return;
    char line[96];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    active->note(line);
}

void keepServing() {
    feedAndServe();
}

bool joinStation(const char* ssid, const char* password, uint32_t timeoutMs) {
    // One attempt: no background retries hopping the radio (and the SoftAP)
    // between channels, and a rejected password or a missing network ends
    // the wait as soon as the driver reports it instead of at the timeout.
    // A wrong password ends in a plain STA_DISCONNECTED (status 6), so the
    // first disconnect event after begin() ends the attempt.
    static volatile int disconnectReason;
    disconnectReason = 0;
    const wifi_event_id_t watch = WiFi.onEvent(
        [](arduino_event_id_t, arduino_event_info_t info) {
            disconnectReason = info.wifi_sta_disconnected.reason ? info.wifi_sta_disconnected.reason : -1;
        },
        ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.setAutoReconnect(false);
    if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);  // the hotspot keeps running
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.begin(ssid, password[0] ? password : nullptr);
    const uint32_t start = millis();
    wl_status_t status = WiFi.status();
    while (status != WL_CONNECTED && !disconnectReason && millis() - start < timeoutMs) {
        feedAndServe();
        delay(50);
        status = WiFi.status();
    }
    WiFi.removeEvent(watch);
    portalNote("portal join=%d status=%d reason=%d ms=%lu", status == WL_CONNECTED, int(status),
         int(disconnectReason), static_cast<unsigned long>(millis() - start));
    if (status == WL_CONNECTED) return true;
    WiFi.disconnect(false);  // keep the SoftAP up for another try
    return false;
}

}  // namespace portal
