// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Shared constants, devices and loop-owned state.
//
// Part of lcd43b.ino's single translation unit: included only by lcd43b.ino,
// inside its anonymous namespace, in the order listed there. Earlier parts
// provide the declarations later parts use. Do not include it anywhere else.
#pragma once

#ifdef VENTANAL_LCD_DIAG_PANEL
// Underrun-stress builds (fix-r3-flicker.md): refresh every 30 s so a
// short capture sees several Wi-Fi download + LittleFS commit cycles
// against the live panel, and observe the VSYNC cadence (below).
constexpr uint32_t REFRESH_MS = 30 * 1000UL;
#else
constexpr uint32_t REFRESH_MS = 10 * 60 * 1000UL;
#endif
constexpr uint32_t TOUCH_WAKE_MS = 60000;
constexpr uint32_t TOUCH_POLL_MS = 10;
constexpr uint32_t WORKER_STACK_BYTES = 16384;

Ch422g expander{Wire};
Gt911 touch{Wire};
Panel panel;
lcd::TouchPoller touchPoller{touch};
Preferences prefs;
Preferences settingsPrefs; // exclusively opened/closed by settings persistence
lcd::DeviceSettings settings=lcd::defaultSettings(lcd::POWER_SOURCE_DETECTABLE);
lcd::SettingsSession settingsPage;
bool settingsDirty = false;
int settingsReturnSlot=0;
bool settingsRefreshPending=false;
bool installationReady = false;
// Wi-Fi setup (app_setup.h); declared here for the earlier parts that route to it.
bool setupActive = false;
bool setupQueued = false;
bool showSetup();
void setupTap(int x, int y);
void setupPoll(uint32_t now);
bool enterSetup(lcd::SetupReason why);
bool updatesPage = false;
bool checkQueued = false;
bool refreshCheckingUpdates = false;
lcd::CheckResult githubStatus = lcd::CheckResult::Never;
lcd::UpdateBoot updateBoot;
lcd::UpdateOffer hostOffer, githubOffer, queuedOffer;
lcd::UpdateSource installQueued = lcd::UpdateSource::None;
lcd::GithubCheck githubStaging;
int64_t githubLastCheck = 0;
int64_t githubLastAutoFailure = 0;
int64_t standaloneGithubAttempt = 0;
std::atomic<bool> updateManifestVerified{false};
char refreshOtaReport[otaReportCapacity]{};
char refreshOtaApproval[otaApprovalCapacity]{};
const char* updateNotice = "";

// This board exposes no battery ADC. Keep calibration gated by both hardware
// capability and a valid measurement; never synthesize a full-cell reading.
int settingsBatteryMv() { return -1; }
bool settingsBatteryButton() {
    if constexpr(!lcd::BATTERY_ADC_AVAILABLE) return false;
    return settingsBatteryMv()>0;
}

lcd::RefreshState state;   // manifest, cache ids, offline flag
uint8_t* residentFrames[lcd::SCREEN_SLOTS]{};
uint8_t* overrideFrame = nullptr;
bool ownOverrideFrame = false;
lcd::ResidentCache refreshCache;
volatile bool refreshBusy = false;
bool mounted = false;
lcd::CacheLocation persistedFrames[lcd::SCREEN_SLOTS];
lcd::DeferredSave deferredFrames[lcd::SCREEN_SLOTS];
lcd::DeferredSave deferredOverride;
int overrideSource = -1;  // persisted spare to copy if no wire payload is in RAM
lcd::DeferredSelection selectionSave;
int screenIndex = 0;       // current screen within the screen list
int spareCursor = -1;      // legacy selection key; OwnerState owns swapping
OwnerState owner;
uint32_t skipRevision=0, sentSkipRevision=0;
char refreshSkips[280]{};

// Long-press feedback (host-pinned in overlay.h): REFRESHING shows on the
// gesture, UP TO DATE ~2 s when nothing changed, OFFLINE styling when the
// fetch failed; expiry redraws the cached frame clean.
lcd::RefreshLabelState refreshLabel;
ventanal::ContentStaleness contentStaleness{};
uint32_t weatherFrameEpoch = 0;

lcd::PowerSession powerSession;
// There is no voltage or VBUS signal on this board. No invented reading.
lcd::BatteryLevel powerLevel() {
    const int mv=settingsBatteryMv();
    return lcd::batteryLevel(mv,lcd::batteryPercent(mv,settings.batteryFullMv));
}
uint32_t powerPollMs() {
#ifdef VENTANAL_LCD_DIAG_PANEL
    return REFRESH_MS;
#else
    return lcd::pollIntervalMs(powerSession.screenOn,powerLevel());
#endif
}
bool backlightState = false;
bool backlightKnown = false;
bool quietBlanking = false;
// Sole authority on whether the backlight may be lit (display_owner.h).
lcd::DisplayOwner displayOwner;
bool updateScreenShowing = false;  // the Updating notice is lit even in quiet hours
uint32_t touchUntilMs = 0;
uint32_t lastFetchMs = 0;
lcd::WifiRetry wifiRetry;
lcd::WifiRetry updateRetry;
uint32_t lastTouchPollMs = 0;
bool lastTouched = false;
uint32_t fetchCounter = 0;

#ifdef VENTANAL_LCD_DIAG_GESTURES
// One-shot diagnostic gesture replay (see diag.h), armed in setup() and
// disabled once the synthetic sequence completes.
bool diagActive = false;
uint32_t diagStartMs = 0;
#endif
#ifdef VENTANAL_LCD_DIAG_TOUCH
// Rate-limited raw-report logging for touch captures: every report change
// plus at most one line per second while a finger is held.
uint32_t lastRawLogMs = 0;
lcd::TouchSample lastRawSample{};
#endif
