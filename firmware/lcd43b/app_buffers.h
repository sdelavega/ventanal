// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Clock, status and local-page buffers and pixel retirement.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there. Earlier parts
// provide the declarations later parts use. Do not include it anywhere else.
#pragma once
bool setBacklight(bool on);
void backlightRefresh();
void reclaimCacheMemory(size_t bytes);

// Hold the screen dark for this scope through the display owner. Release
// does not relight by itself; the next backlightRefresh() decides, as the
// direct setBacklight(false) calls this replaces did.
class HeldDark {
public:
    explicit HeldDark(lcd::DarkReason reason, bool needed = true) : reason_(reason), held_(needed) {
        if (!held_) return;
        displayOwner.acquire(reason_, millis());
        backlightRefresh();
    }
    ~HeldDark() { if (held_) displayOwner.release(reason_); }
    HeldDark(const HeldDark&) = delete;
    HeldDark& operator=(const HeldDark&) = delete;
private:
    lcd::DarkReason reason_;
    bool held_;
};

// Loop owns these buffers. A scene's packed overlays stay immutable until
// the bounce callback has switched away from that scene.
constexpr size_t ClockPixels = size_t(lcd::ART_CORNER.w) * lcd::ART_CORNER.h;
constexpr int StatusWidth = 300;
constexpr int OfflineHeight = 62;
constexpr int RefreshHeight = 38;
struct StatusPixels {
    uint16_t offline[StatusWidth * OfflineHeight];
    uint16_t refresh[StatusWidth * RefreshHeight];
};
StatusPixels* statusPixels[2]{};
uint16_t* clockPixels[2]{};
constexpr int CanvasRows = 40;
constexpr size_t CanvasBytes = lcd::WIDTH * CanvasRows * sizeof(uint16_t);
uint16_t* canvas = nullptr;
lcd::LocalPage* localPages[2]{};
int displayedLocal = -1;
int clockSlot = -1;

bool clockBuffersReady() { return clockPixels[0] && clockPixels[1]; }

const uint16_t* framePixels(const uint8_t* frame) {
    return frame ? reinterpret_cast<const uint16_t*>(frame + lcd::HEADER_BYTES) : nullptr;
}

void retirePixels(const uint16_t* pixels) {
    if (!panel.references(pixels)) return;
    if (panel.waitForPresent() && !panel.references(pixels)) return;
    HeldDark dark(lcd::DarkReason::Retire);
    const int next = panel.prepare();
    if (next >= 0) {
        panel.present(next, {}, 100);
        if (panel.waitForPresent()) return;
    }
    // A broken scan interrupt cannot acknowledge pointer retirement.
    // Stop DMA before any caller writes or frees the old source.
    if (!stopPanel()) {
        lcd::log("panel restart=scanout-retirement-dma-stop-failed\n"); // Flushes when USB is open.
        delay(20);
        ESP.restart();
    }
}

void allocateLocalPages() {
    canvas = static_cast<uint16_t*>(ps_malloc(CanvasBytes));
    for (auto& page : localPages)
        page = static_cast<lcd::LocalPage*>(ps_malloc(sizeof(lcd::LocalPage)));
    lcd::log("local canvas=%u packed_each=%u allocated=%d\n", unsigned(CanvasBytes),
             unsigned(sizeof(lcd::LocalPage)), canvas && localPages[0] && localPages[1]);
}
