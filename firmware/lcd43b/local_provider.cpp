// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Standalone refresh: with no host configured, the board fetches its own
// weather (Open-Meteo, NWS), draws the screen (local/render_weather.h) and
// hands the frame over exactly as a downloaded host frame is handed over, so
// the cache, owner state, skips, overlays and power code are unchanged.
//
// So far weather only: cached art and spares stay as they are, and the solar
// screen is dropped until the board can draw it.

#include "local_provider.h"

#include "../common/https_get.h"
#include "installation.h"
#include "local/render_weather.h"
#include "log.h"
#include "scanout_diag.h"
#include "sha256.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <memory>

namespace lcd {
namespace {

Watchdog* fetchWatchdog = nullptr;
void feedFetch() {
    if (fetchWatchdog) fetchWatchdog->feed();
}

struct PsramBuffer {
    char* data = nullptr;
    size_t cap = 0;
    explicit PsramBuffer(size_t bytes) : data(static_cast<char*>(ps_malloc(bytes))), cap(data ? bytes : 0) {}
    ~PsramBuffer() { free(data); }
};

// Alerts survive a failed NWS request: the screen keeps the last good set.
local::LocalAlert lastAlerts[4];
int lastAlertCount = 0;

bool fetchWeather(local::LocalWeather& weather, time_t now, Watchdog& watchdog) {
    PsramBuffer body(48 * 1024);
    if (!body.data) return false;
    char url[512];
    snprintf(url, sizeof url,
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m"
             "&hourly=temperature_2m,precipitation_probability"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
             "&timezone=auto&forecast_days=7%s",
             double(installation.latitude), double(installation.longitude),
             installation.units ? "" : "&temperature_unit=fahrenheit&wind_speed_unit=mph");
    fetchWatchdog = &watchdog;
    const uint32_t started = millis();
    const HttpsResult got = httpsGet(url, body.data, body.cap, 20000, feedFetch);
    const bool parsed = got.status == 200 && !got.truncated && local::parseOpenMeteo(body.data, now, weather);
    log("standalone weather status=%d bytes=%u parsed=%d ms=%lu\n", got.status, unsigned(got.length), parsed,
        static_cast<unsigned long>(millis() - started));
    return parsed;
}

void fetchAlerts(Watchdog& watchdog) {
    PsramBuffer body(160 * 1024);
    if (!body.data) return;
    char url[160];
    snprintf(url, sizeof url, "https://api.weather.gov/alerts/active?point=%.4f,%.4f",
             double(installation.latitude), double(installation.longitude));
    fetchWatchdog = &watchdog;
    const uint32_t started = millis();
    const HttpsResult got = httpsGet(url, body.data, body.cap, 20000, feedFetch);
    local::LocalAlert alerts[4];
    const int count = local::parseNwsAlerts(got.truncated ? -1 : got.status, body.data, alerts, 4);
    if (count >= 0) {
        for (int i = 0; i < count; ++i) lastAlerts[i] = alerts[i];
        lastAlertCount = count;
    }
    log("standalone alerts status=%d bytes=%u count=%d kept=%d ms=%lu\n", got.status, unsigned(got.length), count,
        lastAlertCount, static_cast<unsigned long>(millis() - started));
}

void frameId(const uint8_t* frame, char (&id)[17]) {
    Sha256 hash;
    hash.update(frame, FRAME_BYTES);
    const uint8_t* digest = hash.digest();
    for (int i = 0; i < 8; ++i) snprintf(id + 2 * i, 3, "%02x", digest[i]);
}

// Keep a slot exactly as it is cached (art and spares, until the board
// fetches its own).
void keepSlot(int slot, FrameInfo& info, const char currentIds[SCREEN_SLOTS][17], RefreshResult& result) {
    info.present = currentIds[slot][0] != '\0';
    strlcpy(info.id, currentIds[slot], sizeof info.id);
    if (!info.present) return;
    strlcpy(result.frameIds[slot], currentIds[slot], sizeof result.frameIds[slot]);
    result.frameOk[slot] = true;
}

}  // namespace

void runLocalRefresh(const char currentIds[SCREEN_SLOTS][17], RefreshResult& result, Watchdog& watchdog,
                     const ResidentCache& cache) {
    result = RefreshResult{};
    result.wifiOk = connectForUpdates(watchdog);
    result.timeOk = time(nullptr) >= 1704067200;  // 2024-01-01: the clock is set
    if (!result.wifiOk || !result.timeOk) {
        log("standalone refresh wifi=%d time=%d\n", result.wifiOk, result.timeOk);
        return;
    }
    DownloadPower radio;
    const time_t now = time(nullptr);

    DeviceManifest& m = result.manifest;
    m.clock24h = installation.clock24h;
    m.quietStart = installation.quietStart;
    m.quietEnd = installation.quietEnd;
    keepSlot(0, m.screens[0], currentIds, result);
    for (int i = 0; i < SPARE_SLOTS && currentIds[3 + i][0]; ++i) {
        keepSlot(3 + i, m.spares[i], currentIds, result);
        m.spareCount = uint8_t(i + 1);
    }
    for (int i = m.spareCount; i < SPARE_SLOTS; ++i) {
        result.dropSlot[3 + i] = true;
        if (currentIds[3 + i][0]) cache.publish(3 + i, nullptr, {});
    }
    m.screens[2].present = false;  // solar: not drawn on the board yet
    result.dropSlot[2] = true;
    if (currentIds[2][0]) cache.publish(2, nullptr, {});
    result.manifestOk = true;

    auto weather = std::make_unique<local::LocalWeather>();
    const bool fetched = fetchWeather(*weather, now, watchdog);
    watchdog.feed();
    if (!fetched) {
        result.frameFailed[1] = true;  // keep the old frame, report the failure
        m.screens[1].present = currentIds[1][0] != '\0';
        strlcpy(m.screens[1].id, currentIds[1], sizeof m.screens[1].id);
        return;
    }
    if (cache.manifestVerified) cache.manifestVerified();  // the network works: an update may confirm
    fetchAlerts(watchdog);
    watchdog.feed();

    auto* bytes = static_cast<uint8_t*>(ps_malloc(FRAME_BYTES));
    if (!bytes && cache.reclaimWireMemory) {
        cache.reclaimWireMemory(FRAME_BYTES);
        bytes = static_cast<uint8_t*>(ps_malloc(FRAME_BYTES));
    }
    if (!bytes || !cache.staging) {
        free(bytes);
        log("standalone render skipped=no-psram\n");
        result.frameFailed[1] = true;
        return;
    }
    FrameBody body{std::shared_ptr<uint8_t>(bytes, free), FRAME_BYTES};
    const uint8_t header[HEADER_BYTES] = {'V', 'N', 'R', '1', 0x20, 0x03, 0xE0, 0x01};
    memcpy(bytes, header, HEADER_BYTES);
    local::WeatherScreen screen;
    screen.metric = installation.units == 1;
    screen.clock24h = installation.clock24h;
    screen.place = installation.place;
    screen.latitude = installation.latitude;
    screen.longitude = installation.longitude;
    const uint32_t started = millis();
    {
        ScanScope activity(ScanActivity::Inflate);  // a full-frame write, like inflating one
        local::renderWeather(local::Frame{reinterpret_cast<uint16_t*>(bytes + HEADER_BYTES)}, *weather, lastAlerts,
                             lastAlertCount, screen, now);
    }
    watchdog.feed();
    FrameInfo& info = m.screens[1];
    frameId(bytes, info.id);
    info.size = FRAME_BYTES;
    info.encoding = FrameEncoding::Raw;
    info.present = true;
    log("standalone render slot=weather id=%s ms=%lu\n", info.id, static_cast<unsigned long>(millis() - started));
    if (!strcmp(currentIds[1], info.id)) {
        strlcpy(result.frameIds[1], info.id, sizeof result.frameIds[1]);
        result.frameOk[1] = true;
        return;
    }
    memcpy(cache.staging, bytes, FRAME_BYTES);
    if (cache.publish(1, &info, body)) {
        strlcpy(result.frameIds[1], info.id, sizeof result.frameIds[1]);
        result.frameOk[1] = true;
    } else {
        result.frameFailed[1] = true;
    }
}

}  // namespace lcd
