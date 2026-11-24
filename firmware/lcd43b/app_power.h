// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Charge-me screen, sleep and power session ticks.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there. Earlier parts
// provide the declarations later parts use. Do not include it anywhere else.
#pragma once
// Never call touch.begin() here: that would drive INT and select an address.
bool probeTouchAfterWake() {
    Wire.beginTransmission(Gt911::PRIMARY);
    const bool answers=Wire.endTransmission()==0;
    uint8_t product[3]={};
    const bool ok=answers && touch.address()==Gt911::PRIMARY
        && touch.peek(0x8140,product,sizeof product)
        && product[0]=='9' && product[1]=='1' && product[2]=='1';
    lcd::log(ok?"gt911 ok after wake\n":"gt911 error after wake\n");
    return ok;
}
void showChargeMe() {
    if(!beginPanel()) {
        lcd::log("power error=panel-resume\n");
        powerSession.chargeFailed(millis());
        backlightRefresh();
        return;
    }
    const int local = drawLocalScene([](lcd::SettingsSurface band) {
        lcd::drawSettingsChrome(band, false);
        band.text(StatusFont, "CHARGE ME", 280, 240, lcd::rgb565(221,68,68));
    });
    clockSlot = -1;
    if (local >= 0) presentScreen(nullptr, false, nullptr, local);
    else lcd::log("power error=charge-allocation\n");
    powerSession.showCharge(millis());
    backlightRefresh();
    lcd::log("power charge-me seconds=10\n");
}
void restoreTouchWake() {
    powerSession.wakeTouch(millis());
    touchUntilMs=millis()+TOUCH_WAKE_MS;
    lastTouched=false;
    touchPoller.suppressWakeTouch();
    refreshLabel.clear(); logRefreshLabel();
    if(powerLevel()==lcd::BatteryLevel::Critical) showChargeMe();
    else if(beginPanel()) {
        show(screenIndex); // cache + current clock, dimmed, before network I/O
        backlightRefresh();
        lcd::log("power restored brightness=%u screen=%s label=idle\n",
                 settings.brightness,lcd::SLOT_NAMES[screenSlot(screenIndex)]);
    } else lcd::log("power error=panel-resume\n");
}
void sleepPower() {
    HeldDark asleep(lcd::DarkReason::Sleep);
    if (settingsDirty && lcd::saveSettings(settingsPrefs, settings)) settingsDirty = false;
    persistDeferred(false); // selection becomes due on the same transition to dark
    // Let loop() drain one file per pass before entering light sleep.
    if (!settingsPage.active && pendingCacheSave(millis())) {
        powerSession.sleep();
        return;
    }
    if(!panel.stop()) { lcd::log("power error=panel-stop\n"); delay(1000); return; }
    powerSession.sleep();
    // Manual light sleep cannot preserve association. The existing worker
    // reconnects on demand, using its bounded/watchdog-supervised path.
    WiFi.mode(WIFI_OFF);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    uint32_t wait=lcd::pollWaitMs(millis()-lastFetchMs,powerPollMs());
#ifdef VENTANAL_LCD_DIAG_POWER
    if(powerLevel()!=lcd::BatteryLevel::Critical) wait=20000; // capture timer wake
#endif
    esp_err_t armed=gpio_wakeup_enable(GPIO_NUM_4,GPIO_INTR_LOW_LEVEL);
    if(armed==ESP_OK) armed=esp_sleep_enable_gpio_wakeup();
    if(armed==ESP_OK && wait) armed=esp_sleep_enable_timer_wakeup(uint64_t(wait)*1000);
    lcd::log("power sleep timer_ms=%lu critical=%d int=%d\n",
             static_cast<unsigned long>(wait),powerLevel()==lcd::BatteryLevel::Critical,digitalRead(4));
    Serial.flush();
    const int result=lcd::sleepWithUsb(int(armed),[] { return int(esp_light_sleep_start()); },
        [](bool enabled) {
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE && (defined(VENTANAL_LCD_DIAG_POWER) || defined(VENTANAL_LCD_USB_SLEEP_REENUM))
            // Experimental: hardware capture did not recover CDC on this host.
            usb_serial_jtag_ll_phy_enable_pad(enabled);
            if(!enabled) delay(20); // let the host observe detach before clocks stop
#else
            (void)enabled;
#endif
        });
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    gpio_wakeup_disable(GPIO_NUM_4);
    if(result!=ESP_OK) {
        lcd::log("power error=sleep code=%d\n",int(result));
        // Bounded retry while dark; do not claim a successful wake.
        delay(1000);
        return;
    }
    // A rejected/failed sleep has no new wake cause or post-wake probe.
    lcd::networkRecovery.woke();
    const auto reason=esp_sleep_get_wakeup_cause();
    if(powerSession.touchReady || powerSession.probeDue(millis()))
        powerSession.probed(probeTouchAfterWake(),millis());
    if(reason==ESP_SLEEP_WAKEUP_GPIO) {
        lcd::log("power wake reason=touch\n");
        restoreTouchWake();
    } else if(reason==ESP_SLEEP_WAKEUP_TIMER) {
        lcd::log("power wake reason=timer\n");
        powerSession.wakeTimer();
    } else lcd::log("power wake reason=other code=%d\n",int(reason));
}
void tickPower() {
    if(powerSession.probeDue(millis()))
        powerSession.probed(probeTouchAfterWake(),millis());
    // Quiet hours are driven by backlightRefresh(), including touch override.
    const auto action=lcd::decidePower({false,false,powerLevel(),powerSession.idle(millis()),
                                       settings.timeoutMin,false});
    switch(powerSession.step(action,millis(),refreshBusy)) {
    case lcd::PowerStep::ShowCharge: showChargeMe(); break;
    case lcd::PowerStep::Restore: show(screenIndex); backlightRefresh(); break;
    case lcd::PowerStep::Sleep: sleepPower(); break;
    case lcd::PowerStep::None: break;
    }
}
