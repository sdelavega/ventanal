// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Ventanal phase-1 firmware for the Waveshare ESP32-S3-Touch-LCD-4.3B.
// The board is USB powered (no battery, no deep sleep): it fetches the
// per-device manifest every ten minutes and on demand, hash-verifies and
// caches changed VNR1 RGB565 frames in LittleFS, and blits the selected
// screen to the 800x480 panel. Touch: tap right half = next screen, left
// half = previous screen, double tap = next spare artwork, press >= 1.5 s
// = refresh now. The backlight follows the manifest's quiet hours; any
// touch lights it for 60 s during quiet hours. Offline keeps the cached
// screen with a small OFFLINE mark.

#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <ctime>
#include <esp_sntp.h>

#ifdef VENTANAL_LCD_CONFIG
#include VENTANAL_LCD_CONFIG
#elif __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

#include "ch422g.h"
#include "frame.h"
#include "gesture.h"
#include "gt911.h"
#include "manifest.h"
#include "overlay.h"
#include "panel.h"

// Optional private-config keys with safe defaults.
#ifndef VENTANAL_DEVICE_ID
#define VENTANAL_DEVICE_ID "lcd43b"
#endif
#ifndef VENTANAL_TZ
#define VENTANAL_TZ "MST7MDT,M3.2.0,M11.1.0"
#endif

namespace {

constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
constexpr uint32_t NTP_TIMEOUT_MS = 10000;
constexpr uint32_t HTTP_TIMEOUT_MS = 10000;
constexpr uint32_t TRANSFER_BUDGET_MS = 60000;
constexpr uint32_t REFRESH_MS = 10 * 60 * 1000UL;
constexpr uint32_t TOUCH_WAKE_MS = 60000;
constexpr uint32_t TOUCH_POLL_MS = 10;
constexpr size_t CHUNK_BYTES = 32768;

Ch422g expander{Wire};
Gt911 touch{Wire};
Panel panel;
GestureClassifier gestures;
Preferences prefs;

bool backlightState = false;
uint32_t touchUntilMs = 0;
uint32_t lastFetchMs = 0;
uint32_t lastTouchPollMs = 0;
bool lastTouched = false;
bool offline = true;
bool drawn = false;
uint32_t fetchCounter = 0;

lcd::DeviceManifest manifest;

// Screen slots: 0=art, 1=weather, 2=solar, 3..5=spares. Slot names double
// as their LittleFS cache file names.
constexpr uint8_t SLOT_COUNT = 6;
const char* const SLOT_NAMES[SLOT_COUNT] = {"art", "weather", "solar",
                                            "spare0", "spare1", "spare2"};
char cachedIds[SLOT_COUNT][17] = {};

uint8_t screenIndex = 0;   // current screen within the screen list
uint8_t spareCursor = 0;   // next spare shown by a double tap

bool setTouchResetLine(bool asserted) { return expander.setTouchReset(asserted); }
bool setLcdResetLine(bool asserted) { return expander.setLcdReset(asserted); }

// Diagnostic only: report every I2C address that acknowledges a probe.
void scanBus() {
    Serial.print("i2c scan:");
    for (uint8_t address = 1; address < 0x78; ++address) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0) Serial.printf(" %02x", address);
    }
    Serial.println();
}

int screenCount() {
    return 2 + (manifest.screens[2].present ? 1 : 0) + manifest.spareCount;
}

int spareBase() { return 2 + (manifest.screens[2].present ? 1 : 0); }

const lcd::FrameInfo& screenInfo(int index) {
    static const lcd::FrameInfo empty;
    if (index < 3) return manifest.screens[index];
    const int spare = index - spareBase();
    return spare >= 0 && spare < manifest.spareCount ? manifest.spares[spare] : empty;
}

int screenSlot(int index) { return index < 3 ? index : 3 + (index - spareBase()); }

void loadCacheIds() {
    for (int slot = 0; slot < SLOT_COUNT; ++slot) {
        String id = prefs.getString(("id" + String(slot)).c_str(), "");
        strlcpy(cachedIds[slot], id.c_str(), sizeof cachedIds[slot]);
    }
}

void storeCacheId(int slot) {
    prefs.putString(("id" + String(slot)).c_str(), cachedIds[slot]);
}

void loadScreenIndex() {
    screenIndex = prefs.getUChar("screen", 0);
    spareCursor = prefs.getUChar("spare-cursor", 0);
}

void storeScreenIndex() {
    prefs.putUChar("screen", screenIndex);
    prefs.putUChar("spare-cursor", spareCursor);
}

bool wifiConnect() {
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.mode(WIFI_STA);
    WiFi.begin(VENTANAL_WIFI_SSID, VENTANAL_WIFI_PASSWORD);
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS)
        delay(50);
    const bool connected = WiFi.status() == WL_CONNECTED;
    Serial.printf("wifi connected=%d\n", connected);
    return connected;
}

bool syncTime() {
    configTzTime(VENTANAL_TZ, "pool.ntp.org", "time.nist.gov");
    const uint32_t started = millis();
    while (millis() - started < NTP_TIMEOUT_MS) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            Serial.println("time ntp=synced");
            return true;
        }
        delay(100);
    }
    Serial.println("time ntp=unavailable");
    return false;
}

int localMinuteOfDay() {
    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    return local.tm_hour * 60 + local.tm_min;
}

bool fetchManifest() {
    HTTPClient http;
    const String url = String(VENTANAL_HOST_URL) + "/devices/" + VENTANAL_DEVICE_ID
                     + "/manifest.json";
    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    const int status = http.GET();
    if (status != 200 || http.getSize() <= 0 || http.getSize() > 8192) {
        Serial.printf("manifest status=%d\n", status);
        http.end();
        return false;
    }
    const String body = http.getString();
    http.end();
    lcd::DeviceManifest parsed;
    if (!lcd::parseDeviceManifest(body.c_str(), parsed)) {
        Serial.println("manifest parsed=0");
        return false;
    }
    manifest = parsed;
    Serial.printf("manifest parsed=1 screens=%d%d%d spares=%u quiet=%04d-%04d\n",
                  manifest.screens[0].present, manifest.screens[1].present,
                  manifest.screens[2].present, manifest.spareCount,
                  manifest.quietValid() ? manifest.quietStart : -1,
                  manifest.quietValid() ? manifest.quietEnd : -1);
    return true;
}

// Download one frame into a PSRAM staging buffer, hash-verify it against the
// manifest, and only then commit it to its cache file.
bool downloadFrame(const lcd::FrameInfo& info, int slot) {
    uint8_t* buffer = static_cast<uint8_t*>(ps_malloc(lcd::FRAME_BYTES));
    if (!buffer) {
        Serial.println("download error=allocation");
        return false;
    }
    const String url = String(VENTANAL_HOST_URL) + info.url;
    bool ok = false;
    HTTPClient http;
    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    const uint32_t started = millis();
    const int status = http.GET();
    if (status == 200 && http.getSize() == int(lcd::FRAME_BYTES)) {
        size_t got = 0;
        WiFiClient* stream = http.getStreamPtr();
        while (got < lcd::FRAME_BYTES && millis() - started < TRANSFER_BUDGET_MS) {
            const int available = stream->available();
            if (available > 0) {
                const size_t n = size_t(available) > CHUNK_BYTES
                                     ? CHUNK_BYTES
                                     : size_t(available);
                const int read = stream->readBytes(buffer + got, n);
                if (read <= 0) break;
                got += read;
            } else {
                if (!http.connected()) break;
                delay(2);
            }
        }
        ok = got == lcd::FRAME_BYTES
          && lcd::frameVerified(buffer, lcd::FRAME_BYTES, info.sha256);
        Serial.printf("download slot=%s id=%.16s bytes=%u verified=%d ms=%lu\n",
                      SLOT_NAMES[slot], info.id, unsigned(got), ok,
                      static_cast<unsigned long>(millis() - started));
    } else {
        Serial.printf("download slot=%s status=%d size=%d\n", SLOT_NAMES[slot],
                       status, http.getSize());
    }
    http.end();
    if (ok) {
        const String path = String("/") + SLOT_NAMES[slot] + ".vnr";
        const String temporary = path + ".tmp";
        File out = LittleFS.open(temporary, "w");
        if (out) {
            size_t written = 0;
            while (written < lcd::FRAME_BYTES) {
                const size_t n = out.write(buffer + written, lcd::FRAME_BYTES - written);
                if (!n) break;
                written += n;
            }
            out.close();
            ok = written == lcd::FRAME_BYTES && LittleFS.rename(temporary, path);
        } else {
            ok = false;
        }
        LittleFS.remove(temporary);
        if (ok) {
            strlcpy(cachedIds[slot], info.id, sizeof cachedIds[slot]);
            storeCacheId(slot);
        }
    }
    free(buffer);
    return ok;
}

void syncSlot(const lcd::FrameInfo& info, int slot) {
    const String path = String("/") + SLOT_NAMES[slot] + ".vnr";
    const String temporary = path + ".tmp";
    if (!info.present) {
        // The profile dropped this screen; forget any cached frame.
        if (LittleFS.exists(path)) {
            LittleFS.remove(path);
            cachedIds[slot][0] = 0;
            storeCacheId(slot);
            Serial.printf("cache removed slot=%s\n", SLOT_NAMES[slot]);
        }
        LittleFS.remove(temporary);
        return;
    }
    if (!strcmp(cachedIds[slot], info.id) && LittleFS.exists(path)) {
        Serial.printf("download slot=%s skipped=unchanged\n", SLOT_NAMES[slot]);
        return;
    }
    downloadFrame(info, slot);   // failures keep the previous cache
}

// Blit a cached frame (plus overlays) to the panel through a PSRAM buffer.
bool show(int index) {
    const int slot = screenSlot(index);
    const String path = String("/") + SLOT_NAMES[slot] + ".vnr";
    uint16_t* pixels = static_cast<uint16_t*>(
        ps_malloc(size_t(lcd::WIDTH) * lcd::HEIGHT * 2));
    if (!pixels) {
        Serial.println("blit error=allocation");
        return false;
    }
    const uint32_t started = millis();
    bool ok = false;
    if (cachedIds[slot][0] && LittleFS.exists(path)) {
        File file = LittleFS.open(path, "r");
        if (file) {
            uint8_t header[lcd::HEADER_BYTES] = {};
            if (file.read(header, lcd::HEADER_BYTES) == int(lcd::HEADER_BYTES)
                && lcd::headerValid(header)
                && file.read(reinterpret_cast<uint8_t*>(pixels),
                             size_t(lcd::WIDTH) * lcd::HEIGHT * 2)
                       == int(size_t(lcd::WIDTH) * lcd::HEIGHT * 2)) {
                ok = true;
            }
            file.close();
        }
    }
    if (!ok) {
        lcd::drawWaiting(pixels, lcd::WIDTH, lcd::HEIGHT, VENTANAL_HOST_URL);
    } else if (offline) {
        lcd::drawOfflineMark(pixels, lcd::WIDTH, lcd::HEIGHT);
    }
    panel.blit(pixels);
    free(pixels);
    drawn = ok;
    Serial.printf("blit screen=%s id=%.16s cache=%s offline=%d ms=%lu\n",
                  SLOT_NAMES[slot], cachedIds[slot], ok ? "frame" : "waiting",
                  offline, static_cast<unsigned long>(millis() - started));
    return ok;
}

void setBacklight(bool on) {
    if (on == backlightState) return;
    backlightState = on;
    expander.setBacklight(on);
    Serial.printf("backlight on=%d\n", on);
}

void backlightRefresh() {
    const bool quiet = manifest.quietValid()
        && lcd::inQuietMinutes(localMinuteOfDay(), manifest.quietStart, manifest.quietEnd);
    setBacklight(!quiet || millis() < touchUntilMs);
}

void downloadChanged() {
    syncSlot(manifest.screens[0], 0);
    syncSlot(manifest.screens[1], 1);
    syncSlot(manifest.screens[2], 2);
    for (int i = 0; i < manifest.spareCount; ++i) syncSlot(manifest.spares[i], 3 + i);
    for (int i = manifest.spareCount; i < lcd::SPARE_SLOTS; ++i)
        syncSlot(lcd::FrameInfo{}, 3 + i);
}

bool refresh(bool redraw) {
    lastFetchMs = millis();
    wifiConnect();
    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) {
        syncTime();
        ok = fetchManifest();
        if (ok) downloadChanged();
    }
    offline = !ok;
    Serial.printf("refresh fetch=%lu ok=%d offline=%d\n", fetchCounter++, ok, offline);
    // Redraw when asked, when the fetch succeeded, or while nothing valid
    // has ever been drawn (the offline overlay may have changed).
    if (redraw || ok || !drawn) show(screenIndex);
    backlightRefresh();
    return ok;
}

void handleGesture(Gesture gesture, int x, int y) {
    const char* name = gesture == Gesture::Next      ? "next"
                     : gesture == Gesture::Previous  ? "previous"
                     : gesture == Gesture::DoubleTap ? "double"
                                                      : "refresh";
    Serial.printf("gesture=%s x=%d y=%d\n", name, x, y);
    const int count = screenCount();
    switch (gesture) {
    case Gesture::Next:
        screenIndex = (screenIndex + 1) % count;
        show(screenIndex);
        break;
    case Gesture::Previous:
        screenIndex = (screenIndex + count - 1) % count;
        show(screenIndex);
        break;
    case Gesture::DoubleTap:
        if (manifest.spareCount) {
            screenIndex = spareBase() + spareCursor;
            spareCursor = (spareCursor + 1) % manifest.spareCount;
            show(screenIndex);
        }
        break;
    case Gesture::Refresh:
        refresh(true);
        break;
    default:
        break;
    }
    storeScreenIndex();
    backlightRefresh();
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("ventanal lcd43b boot");
    Wire.begin(8, 9);   // SDA=GPIO8, SCL=GPIO9, shared by expander and touch
    Wire.setClock(400000);
    if (!prefs.begin("ventanal", false)) Serial.println("error=nvs-open");
    // Mount the partition named "littlefs" (spiffs subtype); first boot formats.
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs"))
        Serial.println("error=littlefs-mount");
    loadCacheIds();
    loadScreenIndex();
    if (!expander.begin()) Serial.println("error=ch422g-begin");
    expander.setBacklight(false);
    if (!panel.begin(setLcdResetLine)) Serial.println("error=panel-begin");
    if (!touch.begin(4, setTouchResetLine)) {
        Serial.println("error=gt911-begin");
        scanBus();
    } else {
        Serial.printf("touch gt911 address=0x%02x\n", touch.address());
    }
    refresh(true);
}

void loop() {
    const uint32_t now = millis();
    if (now - lastTouchPollMs >= TOUCH_POLL_MS) {
        lastTouchPollMs = now;
        int x = 0, y = 0;
        const bool touched = touch.read(x, y);
        const Gesture gesture = gestures.sample(now, touched, x, y);
        if (touched && !lastTouched) touchUntilMs = now + TOUCH_WAKE_MS;
        lastTouched = touched;
        if (gesture != Gesture::None) handleGesture(gesture, x, y);
    }
    backlightRefresh();
    if (now - lastFetchMs >= REFRESH_MS) refresh(false);
    delay(2);
}