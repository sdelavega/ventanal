// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Standalone refresh: with no host configured, the board fetches its own
// weather (Open-Meteo, NWS) every pass, space weather (NOAA SWPC, NASA SDO)
// every three hours when a call sign is set, and its own art once a local day
// (Cleveland and the Met), draws the frames and hands them over exactly as a
// downloaded host frame is handed over: decoded into the staging buffer, with
// a zlib body for the cache. Cache, owner state, skips, overlays and power
// code are unchanged.

#include "local_provider.h"

#include "../common/https_get.h"
#include "installation.h"
#include "local/art_day.h"
#include "local/render_solar.h"
#include "local/render_weather.h"
#include "local_media.h"
#include "log.h"
#include "scanout_diag.h"
#include "sha256.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <memory>
#include <string>
#include <vector>

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
    PsramBuffer(const PsramBuffer&) = delete;
    PsramBuffer& operator=(const PsramBuffer&) = delete;
};

HttpsResult get(const char* url, PsramBuffer& body, Watchdog& watchdog) {
    fetchWatchdog = &watchdog;
    return httpsGet(url, body.data, body.cap, 20000, feedFetch);
}

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
    const uint32_t started = millis();
    const HttpsResult got = get(url, body, watchdog);
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
    const uint32_t started = millis();
    const HttpsResult got = get(url, body, watchdog);
    local::LocalAlert alerts[4];
    const int count = local::parseNwsAlerts(got.truncated ? -1 : got.status, body.data, alerts, 4);
    if (count >= 0) {
        for (int i = 0; i < count; ++i) lastAlerts[i] = alerts[i];
        lastAlertCount = count;
    }
    log("standalone alerts status=%d bytes=%u count=%d kept=%d ms=%lu\n", got.status, unsigned(got.length), count,
        lastAlertCount, static_cast<unsigned long>(millis() - started));
}

void wireId(const FrameBody& body, char (&id)[17]) {
    Sha256 hash;
    hash.update(body.bytes.get(), body.size);
    const uint8_t* digest = hash.digest();
    for (int i = 0; i < 8; ++i) snprintf(id + 2 * i, 3, "%02x", digest[i]);
}

constexpr uint8_t VNR1[HEADER_BYTES] = {'V', 'N', 'R', '1', 0x20, 0x03, 0xE0, 0x01};

local::Frame stagingFrame(const ResidentCache& cache) {
    memcpy(cache.staging, VNR1, HEADER_BYTES);
    return local::Frame{reinterpret_cast<uint16_t*>(cache.staging + HEADER_BYTES)};
}

// The staging buffer holds a finished frame: compress it and hand it over.
// Returns true when the slot now holds it (or already held it).
bool publishStaged(int slot, FrameInfo& info, const char currentIds[SCREEN_SLOTS][17], RefreshResult& result,
                   Watchdog& watchdog, const ResidentCache& cache) {
    FrameBody body;
    const uint32_t started = millis();
    if (!deflateFrame(cache.staging, body, watchdog) && cache.reclaimWireMemory) {
        cache.reclaimWireMemory(400 * 1024);  // let the loop save queued frames, then try once more
        if (!deflateFrame(cache.staging, body, watchdog)) {
            log("standalone deflate slot=%s ok=0\n", SLOT_NAMES[slot]);
            result.frameFailed[slot] = true;
            return false;
        }
    }
    wireId(body, info.id);
    info.size = body.size;
    info.encoding = FrameEncoding::Zlib;
    info.present = true;
    log("standalone frame slot=%s id=%s bytes=%u deflate_ms=%lu\n", SLOT_NAMES[slot], info.id, unsigned(body.size),
        static_cast<unsigned long>(millis() - started));
    if (!strcmp(currentIds[slot], info.id) || cache.publish(slot, &info, body)) {
        strlcpy(result.frameIds[slot], info.id, sizeof result.frameIds[slot]);
        result.frameOk[slot] = true;
        return true;
    }
    result.frameFailed[slot] = true;
    return false;
}

// Keep a slot exactly as it is cached.
void keepSlot(int slot, FrameInfo& info, const char currentIds[SCREEN_SLOTS][17], RefreshResult& result) {
    info.present = currentIds[slot][0] != '\0';
    strlcpy(info.id, currentIds[slot], sizeof info.id);
    if (!info.present) return;
    strlcpy(result.frameIds[slot], currentIds[slot], sizeof result.frameIds[slot]);
    result.frameOk[slot] = true;
}

// ---- solar -------------------------------------------------------------------

constexpr char SwpcBase[] = "https://services.swpc.noaa.gov/";
constexpr char SdoUrl[] = "https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_0171.jpg";
constexpr int64_t SolarSeconds = 3 * 3600;

// RAM only: after a restart the screen is drawn again on the first pass.
int64_t solarDrawnAt = 0;
std::vector<std::string> lastSolarAlerts;
// The day's SDO image (the server fetches it once a UTC day too).
uint8_t* sunJpeg = nullptr;
size_t sunLength = 0;
int64_t sunDay = -1;

bool solarProduct(const char* path, PsramBuffer& body, Watchdog& watchdog, bool allowCut = false) {
    char url[128];
    snprintf(url, sizeof url, "%s%s", SwpcBase, path);
    const HttpsResult got = get(url, body, watchdog);
    log("standalone solar %s status=%d bytes=%u\n", path, got.status, unsigned(got.length));
    return got.status == 200 && (allowCut || !got.truncated);
}

bool fetchSolar(local::LocalSolar& s, Watchdog& watchdog) {
    PsramBuffer body(256 * 1024);  // the X-ray file is the largest: about 160 KB
    if (!body.data) return false;
    const bool ok = solarProduct("text/daily-solar-indices.txt", body, watchdog)
        && local::parseDailyIndices(body.data, s.sfi, s.ssn)
        && solarProduct("products/noaa-planetary-k-index.json", body, watchdog) && local::parseKp(body.data, s)
        // A cut-off X-ray file still holds the readings before the cut.
        && solarProduct("json/goes/primary/xrays-6-hour.json", body, watchdog, true) && local::parseXray(body.data, s.xray)
        && solarProduct("products/noaa-scales.json", body, watchdog) && local::parseScales(body.data, s)
        && solarProduct("text/3-day-forecast.txt", body, watchdog);
    if (!ok) return false;
    local::parseForecastKp(body.data, s.kpMax);
    std::vector<local::AlertRow> rows;
    if (solarProduct("products/alerts.json", body, watchdog) && local::parseAlertRows(body.data, rows))
        lastSolarAlerts = local::activeAlerts(rows, time(nullptr));  // otherwise the last good set
    s.alerts = lastSolarAlerts;
    return true;
}

void fetchSun(time_t now, Watchdog& watchdog) {
    const int64_t day = int64_t(now) / 86400;
    if (sunJpeg && sunDay == day) return;
    PsramBuffer body(512 * 1024);
    if (!body.data) return;
    const HttpsResult got = get(SdoUrl, body, watchdog);
    log("standalone sun status=%d bytes=%u\n", got.status, unsigned(got.length));
    if (got.status != 200 || got.truncated || !got.length) return;  // keep yesterday's
    auto* copy = static_cast<uint8_t*>(heap_caps_malloc(got.length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) return;
    memcpy(copy, body.data, got.length);
    heap_caps_free(sunJpeg);
    sunJpeg = copy;
    sunLength = got.length;
    sunDay = day;
}

bool showsSolar() {
    return installation.callSign[0] && installation.callState != uint8_t(CallState::Rejected);
}

// Space weather every three hours; otherwise the cached frame stays.
void refreshSolar(time_t now, const char currentIds[SCREEN_SLOTS][17], RefreshResult& result, Watchdog& watchdog,
                  const ResidentCache& cache) {
    FrameInfo& info = result.manifest.screens[2];
    if (currentIds[2][0] && solarDrawnAt && int64_t(now) - solarDrawnAt < SolarSeconds) {
        keepSlot(2, info, currentIds, result);
        return;
    }
    auto solar = std::make_unique<local::LocalSolar>();
    if (!fetchSolar(*solar, watchdog) || !cache.staging) {
        keepSlot(2, info, currentIds, result);
        result.frameFailed[2] = true;
        result.frameOk[2] = false;
        return;
    }
    fetchSun(now, watchdog);
    local::SolarScreen screen;
    screen.callSign = installation.callSign;
    screen.latitude = installation.latitude;
    screen.longitude = installation.longitude;
    const uint32_t started = millis();
    bool sun = false;
    {
        ScanScope activity(ScanActivity::Inflate);
        local::renderSolar(stagingFrame(cache), *solar, screen, now, [&](local::Frame f) {
            return sun = sunJpeg && drawSunJpeg(sunJpeg, sunLength, f, watchdog);
        });
    }
    log("standalone render slot=solar sun=%d ms=%lu\n", sun, static_cast<unsigned long>(millis() - started));
    if (publishStaged(2, info, currentIds, result, watchdog, cache)) solarDrawnAt = now;
}

// ---- art ---------------------------------------------------------------------

constexpr char LocalNamespace[] = "ventanal-local";
// Standalone keeps two spares: the third spare's frame memory (768 KB) stays
// free as room for the daily art work (an image and the resampler band).
// Freeing a frame and allocating it back later fragments PSRAM until no
// 768 KB block is left, so the room is set aside for good instead.
constexpr int StandaloneSpares = 2;
constexpr int ReservedSpareSlot = 3 + StandaloneSpares;  // slot 5, "spare2"
constexpr int ArtWant = 1 + StandaloneSpares;
constexpr uint32_t ArtRetrySeconds = 3600;

struct ArtState {
    uint32_t day = 0;          // YYYYMMDD of the set on screen
    uint32_t failedAt = 0;     // epoch of the last failed attempt
    local::SeenRing seen;
};

ArtState loadArtState() {
    ArtState s;
    Preferences prefs;
    if (prefs.begin(LocalNamespace, true)) {
        s.day = prefs.getUInt("artday", 0);
        s.failedAt = prefs.getUInt("artfail", 0);
        if (prefs.getBytesLength("seen") == sizeof s.seen) prefs.getBytes("seen", &s.seen, sizeof s.seen);
        prefs.end();
    }
    return s;
}

void saveArtState(const ArtState& s) {
    Preferences prefs;
    if (!prefs.begin(LocalNamespace, false)) return;
    prefs.putUInt("artday", s.day);
    prefs.putUInt("artfail", s.failedAt);
    prefs.putBytes("seen", &s.seen, sizeof s.seen);
    prefs.end();
}

const char* deviceId() {
    static char id[8] = "";
    if (!id[0]) {
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(id, sizeof id, "%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    return id;
}

int searchArt(const char* term, local::ArtCandidate* pool, int max, Watchdog& watchdog) {
    PsramBuffer body(96 * 1024);
    if (!body.data) return 0;
    char url[320];
    int n = 0;
    local::clevelandSearchUrl(term, 12, url, sizeof url);
    HttpsResult got = get(url, body, watchdog);
    const int cleveland = got.status == 200 && !got.truncated ? local::parseClevelandSearch(body.data, pool, max) : -1;
    if (cleveland > 0) n += cleveland;
    local::metSearchUrl(term, 10, url, sizeof url);
    got = get(url, body, watchdog);
    uint32_t ids[10];
    const int found = got.status == 200 && !got.truncated ? local::parseMetSearchIds(body.data, ids, 10) : 0;
    int met = 0;
    for (int i = 0; i < found && i < 6 && n < max; ++i) {  // at most six object lookups
        local::metObjectUrl(ids[i], url, sizeof url);
        got = get(url, body, watchdog);
        if (got.status == 200 && !got.truncated && local::parseMetObject(body.data, pool[n])) {
            ++n;
            ++met;
        }
    }
    log("standalone art search term=\"%s\" cleveland=%d met=%d/%d\n", term, cleveland, met, found);
    return n;
}

// The day's set, if it is due: art screen (slot 0) and spares (slots 3..).
// Returns how many frames it placed, or -1 when it did not run.
int refreshArt(time_t now, const char currentIds[SCREEN_SLOTS][17], RefreshResult& result, Watchdog& watchdog,
               const ResidentCache& cache) {
    struct tm local {};
    localtime_r(&now, &local);
    local::LocalDate today;
    today.year = int16_t(local.tm_year + 1900);
    today.month = uint8_t(local.tm_mon + 1);
    today.day = uint8_t(local.tm_mday);
    const uint32_t day = uint32_t(today.year) * 10000 + today.month * 100 + today.day;
    ArtState state = loadArtState();
#ifdef VENTANAL_LCD_DIAG_STANDALONE_SAMPLE
    static bool forced = false;  // diagnostic runs: a new set on the first pass after boot
    state.failedAt = 0;
    if (!forced) state.day = 0;
    forced = true;
#endif
    const bool haveArt = currentIds[0][0] != '\0';
    if ((state.day == day && haveArt) || (state.failedAt && uint32_t(now) - state.failedAt < ArtRetrySeconds))
        return -1;

    const char* term = local::dayTerm(today, deviceId());
    auto pool = std::unique_ptr<local::ArtCandidate[]>(new (std::nothrow) local::ArtCandidate[24]);
    if (!pool) return -1;
    const int found = searchArt(term, pool.get(), 24, watchdog);
    int order[24];
    const int ranked = local::pickCandidates(pool.get(), found, state.seen, local::daySeed(today, deviceId()), found, order);

    static const int slots[ArtWant] = {0, 3, 4};
    int placed = 0;
    for (int i = 0; i < ranked && placed < ArtWant && cache.staging; ++i) {
        const local::ArtCandidate& c = pool[size_t(order[i])];
        const local::ArtCredit credit{c.title, c.artist, c.date, c.museum};
        const uint32_t started = millis();
        // The cache body is the credit and the JPEG (VNA1): download straight
        // into it behind the header, compose into staging, keep the body.
        const size_t header = artBodyHeaderBytes(credit);
        // Room for a whole image (up to 500 KB) beside the resampler's row band
        // (<= ~200 KB); otherwise let the loop save queued frames first.
        constexpr size_t ImageCap = 500 * 1024, Work = 224 * 1024;
        size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (room < header + Work + ImageCap && cache.reclaimWireMemory) {
            cache.reclaimWireMemory(header + Work + ImageCap);
            room = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        const size_t cap = std::min(ImageCap, room > header + Work + 64 * 1024 ? room - header - Work : size_t(0));
        auto* bytes = cap ? static_cast<uint8_t*>(heap_caps_malloc(header + cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
                          : nullptr;
        if (!bytes) {
            log("standalone art memory=short largest=%u\n", unsigned(room));
            break;
        }
        fetchWatchdog = &watchdog;
        const HttpsResult got = httpsGet(c.imageUrl, reinterpret_cast<char*>(bytes + header), cap, 45000, feedFetch);
        bool composed = false;
        if (got.status == 200 && !got.truncated) {
            ScanScope activity(ScanActivity::Inflate);  // a full-frame write, like inflating one
            composed = composeArtJpeg(bytes + header, got.length, stagingFrame(cache), credit, watchdog);
        }
        log("standalone art piece=%s:%s status=%d bytes=%u truncated=%d composed=%d ms=%lu\n", c.source, c.objectId,
            got.status, unsigned(got.length), got.truncated, composed, static_cast<unsigned long>(millis() - started));
        if (!composed) {
            heap_caps_free(bytes);
            continue;
        }
        writeArtBodyHeader(bytes, credit);
        auto* shrunk = static_cast<uint8_t*>(heap_caps_realloc(bytes, header + got.length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        FrameBody body{std::shared_ptr<uint8_t>(shrunk ? shrunk : bytes, heap_caps_free), header + got.length};
        const int slot = slots[placed];
        FrameInfo& info = slot == 0 ? result.manifest.screens[0] : result.manifest.spares[slot - 3];
        wireId(body, info.id);
        info.size = body.size;
        info.encoding = FrameEncoding::Zlib;  // not a raw frame (the loader tells VNA1 by its magic)
        info.present = true;
        if (!strcmp(currentIds[slot], info.id) || cache.publish(slot, &info, body)) {
            strlcpy(result.frameIds[slot], info.id, sizeof result.frameIds[slot]);
            result.frameOk[slot] = true;
            state.seen.add(local::candidateKey(c));
            ++placed;
        } else {
            result.frameFailed[slot] = true;
        }
    }

    if (placed) {
        state.day = day;
        state.failedAt = 0;
    } else {
        state.failedAt = uint32_t(now);
    }
    saveArtState(state);
    log("standalone art day=%lu placed=%d of %d candidates\n", static_cast<unsigned long>(day), placed, found);
    return placed;
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

    // The reserved spare's memory is the art work room (see StandaloneSpares).
    if (cache.releaseFrame) cache.releaseFrame(ReservedSpareSlot);

    DeviceManifest& m = result.manifest;
    m.clock24h = installation.clock24h;
    m.quietStart = installation.quietStart;
    m.quietEnd = installation.quietEnd;
    if (!showsSolar()) {  // the solar screen is for licensed amateurs
        m.screens[2].present = false;
        result.dropSlot[2] = true;
        if (currentIds[2][0]) cache.publish(2, nullptr, {});
        solarDrawnAt = 0;
    }
    result.manifestOk = true;

    // Weather first: it changes every pass and is quick.
    {
        auto weather = std::make_unique<local::LocalWeather>();
        if (!fetchWeather(*weather, now, watchdog)) {
            result.frameFailed[1] = true;  // keep the old frame, report the failure
            keepSlot(1, m.screens[1], currentIds, result);
            result.frameOk[1] = false;
        } else {
            if (cache.manifestVerified) cache.manifestVerified();  // the network works: an update may confirm
            fetchAlerts(watchdog);
            if (!cache.staging) {
                result.frameFailed[1] = true;
            } else {
                local::WeatherScreen screen;
                screen.metric = installation.units == 1;
                screen.clock24h = installation.clock24h;
                screen.place = installation.place;
                screen.latitude = installation.latitude;
                screen.longitude = installation.longitude;
                const uint32_t started = millis();
                {
                    ScanScope activity(ScanActivity::Inflate);
                    local::renderWeather(stagingFrame(cache), *weather, lastAlerts, lastAlertCount, screen, now);
                }
                log("standalone render slot=weather ms=%lu\n", static_cast<unsigned long>(millis() - started));
                publishStaged(1, m.screens[1], currentIds, result, watchdog, cache);
            }
        }
    }
    watchdog.feed();

    if (showsSolar()) refreshSolar(now, currentIds, result, watchdog, cache);
    watchdog.feed();

    // Art once a local day; otherwise keep the cached set.
    const int placed = cache.staging ? refreshArt(now, currentIds, result, watchdog, cache) : -1;
    if (placed <= 0) {
        keepSlot(0, m.screens[0], currentIds, result);
        for (int i = 0; i < StandaloneSpares && currentIds[3 + i][0]; ++i) {
            keepSlot(3 + i, m.spares[i], currentIds, result);
            m.spareCount = uint8_t(i + 1);
        }
    } else {
        m.spareCount = uint8_t(placed - 1);
        if (!m.screens[0].present) keepSlot(0, m.screens[0], currentIds, result);
    }
    for (int i = m.spareCount; i < SPARE_SLOTS; ++i) {
        result.dropSlot[3 + i] = true;
        if (currentIds[3 + i][0]) cache.publish(3 + i, nullptr, {});
    }
}

}  // namespace lcd
