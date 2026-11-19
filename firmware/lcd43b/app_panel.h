// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Screen list helpers and RGB panel start, stop and scan-out restart.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there. Earlier parts
// provide the declarations later parts use. Do not include it anywhere else.
#pragma once
// Screen list helpers over the live (persisted or freshly fetched) shape.
bool solarPublished() { return state.manifest.screens[2].present; }
int spareCount() { return state.manifest.spareCount; }

bool setTouchResetLine(bool asserted) { return expander.setTouchReset(asserted); }
bool setLcdResetLine(bool asserted) { return expander.setLcdReset(asserted); }

int screenCount() {
    int slots[lcd::SCREEN_SLOTS];
    return lcd::activeSlots(solarPublished(),spareCount(),settings,slots,lcd::SCREEN_SLOTS);
}

int screenSlot(int index) {
    return lcd::screenSlot(index, solarPublished(), spareCount(),settings);
}

void restartScanout(const char* operation) {
    if (!panel.handle()) return;
    const esp_err_t status = esp_lcd_rgb_panel_restart(panel.handle());
    if (status != ESP_OK) lcd::log("panel restart after=%s status=%d\n", operation, int(status));
}

#if defined(VENTANAL_LCD_SCAN_OTHER_CORE) || defined(VENTANAL_LCD_DIAG_F3)
template<class Action>
bool onScanCore(Action action) {
    struct Call { Action action; SemaphoreHandle_t done; bool result; };
    StaticSemaphore_t storage;
    Call call{action, xSemaphoreCreateBinaryStatic(&storage), false};
    const auto run = [](void* arg) {
        auto& call = *static_cast<Call*>(arg);
        call.result = call.action();
        xSemaphoreGive(call.done);
        vTaskDelete(nullptr);
    };
    if (xTaskCreatePinnedToCore(run, "scan-control", 4096, &call, 2, nullptr,
                                1 - xPortGetCoreID()) != pdPASS) return false;
    return xSemaphoreTake(call.done, portMAX_DELAY) == pdTRUE && call.result;
}
#endif

bool stopPanel() {
#ifdef VENTANAL_LCD_DIAG_F3
    if (scanOtherCore) return onScanCore([] { return panel.stop(); });
#endif
#ifdef VENTANAL_LCD_SCAN_OTHER_CORE
    return onScanCore([] { return panel.stop(); });
#else
    return panel.stop();
#endif
}

bool beginPanel() {
    if (panel.handle()) return true;
#ifdef VENTANAL_LCD_DIAG_PANEL
    auto observer = panelVsync;
#else
    esp_lcd_rgb_panel_vsync_cb_t observer = nullptr;
#endif
#ifdef VENTANAL_LCD_DIAG_F3
    if (scanOtherCore) return onScanCore([observer] { return panel.begin(setLcdResetLine, observer); });
#endif
#ifdef VENTANAL_LCD_SCAN_OTHER_CORE
    return onScanCore([observer] { return panel.begin(setLcdResetLine, observer); });
#else
    return panel.begin(setLcdResetLine, observer);
#endif
}

// Deliberately runs outside diag fencing to verify scanout cadence alongside the XIP/64B boot log.
void reportBootScanout(uint32_t now) {
    static const uint32_t start = now;
    static const uint32_t firstFrame = panel.vsyncCount();
    static bool reported = false;
    if (reported || now - start < 1000) return;
    reported = true;
    lcd::log("panel boot scanout vsyncs=%u span_ms=%u pclk_hz=%u bounce_rows=%d drain_us=%u\n",
             unsigned(panel.vsyncCount() - firstFrame), unsigned(now - start),
             Panel::Pclk, Panel::BounceRows, Panel::DrainUs);
}
