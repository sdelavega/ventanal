// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Loading the persisted frame cache into PSRAM at boot.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there. Earlier parts
// provide the declarations later parts use. Do not include it anywhere else.
#pragma once
// Boot runs before the task watchdog is armed for this work.
struct BootWatchdog : lcd::Watchdog {
    void feed() override {}
} bootWatchdog;

bool loadResidentFrame(const char* path, uint8_t* frame, const char* id) {
    if (!mounted || !frame || strlen(id) != 16) return false;
    lcd::ScanScope activity(lcd::ScanActivity::LittleFS);
    File file = LittleFS.open(path, "r");
    const size_t bytes = file ? file.size() : 0;
    if (bytes >= lcd::HEADER_BYTES && bytes <= lcd::FRAME_WIRE_MAX) {
        // An old cache stores a raw VNR1 frame. The zlib wrapper starts
        // with a different magic, even if it happens to have the same size.
        if (bytes == lcd::FRAME_BYTES && file.read(frame, lcd::HEADER_BYTES) == lcd::HEADER_BYTES
            && lcd::headerValid(frame)) {
            if (file.read(frame + lcd::HEADER_BYTES, lcd::PIXEL_BYTES) == lcd::PIXEL_BYTES
                && lcd::cachedFrameValid(frame, id)) return true;
        } else if (file.seek(0)) {
            auto* wire = static_cast<uint8_t*>(ps_malloc(bytes));
            if (wire) {
                // Standalone art is cached as its source (VNA1): compose it again.
                const bool read = file.read(wire, bytes) == bytes && lcd::frameHashMatches(wire, bytes, id, 16);
                bool valid = false;
                if (read) {
                    if (lcd::isArtBody(wire, bytes))
                        valid = lcd::composeArtBody(wire, bytes, frame, bootWatchdog);
                    else if (lcd::isJpegFrame(wire, bytes))
                        valid = lcd::decodeHostJpeg(wire, bytes, frame, bootWatchdog);
                    else
                        valid = lcd::decodeFrame(lcd::FrameEncoding::Zlib, wire, bytes, frame);
                }
                free(wire);
                if (valid) return true;
            }
        }
    }
    lcd::log("cache boot path=%s ok=0\n", path);
    return false;
}

void loadResidentCache() {
    const uint32_t started = millis();
    for (auto& pixels : statusPixels)
        pixels = static_cast<StatusPixels*>(ps_malloc(sizeof(StatusPixels)));
    if (!statusPixels[0] || !statusPixels[1]) lcd::log("status pixels error=allocation\n");
    unsigned allocated = 0;
    unsigned loaded = 0;
    constexpr int firstSpare = lcd::SCREEN_SLOTS - lcd::SPARE_SLOTS;
    for (int slot = 0; slot < lcd::SCREEN_SLOTS; ++slot) {
        residentFrames[slot] = static_cast<uint8_t*>(ps_malloc(lcd::FRAME_BYTES));
        refreshCache.allocated[slot] = residentFrames[slot] != nullptr;
        if (residentFrames[slot]) ++allocated;
        const auto& saved = persistedFrames[slot];
        const String path = lcd::residentPath(slot, saved.alternate);
        if (loadResidentFrame(path.c_str(), residentFrames[slot], saved.id)) {
            strcpy(state.cachedIds[slot], saved.id);
            if (slot == 1) weatherFrameEpoch = saved.frameEpoch;
            ++loaded;
        }
    }
    const bool mainChanged = state.cachedIds[0][0] && strcmp(owner.mainId, state.cachedIds[0]);
    if (mainChanged) {
        owner.mainChanged(state.cachedIds[0]);
        lcd::saveOwner(prefs, owner);
    }
    refreshCache.staging = static_cast<uint8_t*>(ps_malloc(lcd::FRAME_BYTES));
    if (owner.overrideArt.id[0]) {
        for (int slot = firstSpare; slot < lcd::SCREEN_SLOTS; ++slot) {
            if (strcmp(state.cachedIds[slot], owner.overrideArt.id) != 0) continue;
            overrideFrame = residentFrames[slot];
            overrideSource = slot;
            deferredOverride.replace(owner.overrideArt.id, {}, millis());
            break;
        }
        if (!overrideFrame) {
            overrideFrame = static_cast<uint8_t*>(ps_malloc(lcd::FRAME_BYTES));
            ownOverrideFrame = loadResidentFrame("/override.vnr", overrideFrame, owner.overrideArt.id);
            if (!ownOverrideFrame) {
                free(overrideFrame);
                overrideFrame = nullptr;
            }
        }
        if (!overrideFrame) {
            owner.overrideArt = {};
            owner.cursor = -1;
            lcd::saveOwner(prefs, owner);
        }
    }
    if (!beginPanel()) lcd::log("error=panel-begin\n");
    for (auto& pixels : clockPixels) {
        // Keep internal room for connection setup and TLS. If the internal
        // heap is tight, the packed overlay can also be read from PSRAM.
        constexpr size_t internalReserve = 105 * 1024;
        constexpr size_t bytes = ClockPixels * sizeof(uint16_t);
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= internalReserve + bytes)
            pixels = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!pixels) pixels = static_cast<uint16_t*>(ps_malloc(bytes));
    }
#ifdef VENTANAL_LCD_DIAG_PANEL
    constexpr auto internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    lcd::log("panel heap free_internal=%u largest_internal=%u\n",
             unsigned(heap_caps_get_free_size(internal)),
             unsigned(heap_caps_get_largest_free_block(internal)));
#endif
    lcd::log("psram staging=%u override=%u max_wire=%u\n",
             refreshCache.staging ? unsigned(lcd::FRAME_BYTES) : 0,
             ownOverrideFrame ? unsigned(lcd::FRAME_BYTES) : 0, unsigned(lcd::FRAME_WIRE_MAX));
    lcd::log("psram total=%u free=%u largest=%u slots=%u/%u loaded=%u load_ms=%lu\n",
             unsigned(ESP.getPsramSize()), unsigned(ESP.getFreePsram()),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
             allocated, unsigned(lcd::SCREEN_SLOTS), loaded, millis() - started);
}
