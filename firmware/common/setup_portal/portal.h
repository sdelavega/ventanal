// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// The setup portal. PortalCore is the request/poll state machine (pure, host
// tested); SetupPortal wraps it in a WPA2 SoftAP, a catch-all DNS server and
// a web server on the boards.
//
// A submit is answered at once with a "Checking" page that refreshes itself;
// the slow join/locate/save runs in poll(). Joining a network moves the
// SoftAP to that network's channel, so the phone may drop off for a moment:
// the refreshing page simply reconnects instead of losing a pending request.
#pragma once

#include "form.h"
#include "location.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace portal {

struct PortalHooks {
    bool (*join)(const char* ssid, const char* password, uint32_t timeoutMs) = nullptr;
    LocationResult (*locate)(const char* zip) = nullptr;
    // Writes the whole record in one go; false leaves the old record as it was.
    bool (*save)(const PortalFields& fields, const LocationResult& location) = nullptr;
    void (*feed)() = nullptr;  // watchdog (and, on boards, keeps serving the page)
};

enum class PortalOutcome { Running, Saved, TimedOut, Cancelled };

struct Response {
    int code = 200;
    std::string type = "text/html";
    std::string location;  // for redirects
    std::string body;
};

inline constexpr uint32_t portalJoinTimeoutMs = 20000;
inline constexpr uint32_t portalIdleTimeoutMs = 15u * 60u * 1000u;
inline constexpr uint32_t portalRestartDelayMs = 3000;
inline constexpr char portalAddress[] = "http://192.168.4.1/";

class PortalCore {
public:
    PortalCore(PortalHooks hooks, std::vector<std::string> networks, uint32_t nowMs)
        : hooks_(hooks), networks_(std::move(networks)), lastActivity_(nowMs) {}

    Response handleGet(const std::string& path, uint32_t nowMs) {
        lastActivity_ = nowMs;
        if (path != "/" && path != "/save") {  // OS captive-portal probes land on the form
            Response r;
            r.code = 302;
            r.type = "text/plain";
            r.location = portalAddress;
            return r;
        }
        return currentPage();
    }

    Response handlePost(const std::string& path, const char* body, uint32_t nowMs) {
        lastActivity_ = nowMs;
        if (path != "/save" || state_ != State::Form) return currentPage();  // ignore double taps
        status_.clear();
        error_ = parsePortalForm(body, pending_);
        if (error_ == FormError::None) {
            state_ = State::Checking;
            status_ = "Checking the Wi-Fi network\xe2\x80\xa6";
        }
        return currentPage();
    }

    PortalOutcome poll(uint32_t nowMs) {
        if (cancelled_) return PortalOutcome::Cancelled;
        switch (state_) {
        case State::Checking:
            check(nowMs);
            return PortalOutcome::Running;
        case State::Saved:
            return nowMs - savedAt_ >= portalRestartDelayMs ? PortalOutcome::Saved : PortalOutcome::Running;
        case State::Form:
            return nowMs - lastActivity_ >= portalIdleTimeoutMs ? PortalOutcome::TimedOut : PortalOutcome::Running;
        }
        return PortalOutcome::Running;
    }

    void cancel() { cancelled_ = true; }
    const char* status() const { return status_.c_str(); }

private:
    enum class State { Form, Checking, Saved };

    void feed() {
        if (hooks_.feed) hooks_.feed();
    }

    void fail(const std::string& why, FormError error = FormError::None) {
        status_ = why;
        error_ = error;
        state_ = State::Form;
    }

    void check(uint32_t nowMs) {
        feed();
        if (!hooks_.join(pending_.ssid, pending_.password, portalJoinTimeoutMs))
            return fail(std::string("Couldn't join \"") + pending_.ssid + "\". Check the network and its password.");
        feed();
        LocationResult location;
        location.why = LocationResult::Skipped;  // a host-joined board may leave the ZIP empty
        if (pending_.zip[0]) {
            location = hooks_.locate(pending_.zip);
            feed();
            if (!location.ok) {
                switch (location.why) {
                case LocationResult::ZipUnknown:
                    return fail("ZIP not found by the location service.", FormError::Zip);
                case LocationResult::TzUnknown:
                    return fail("Couldn't work out the time zone for that ZIP.", FormError::Zip);
                default:
                    return fail("Joined Wi-Fi but couldn't reach the location service. Try again.");
                }
            }
        }
        if (!hooks_.save(pending_, location)) return fail("Couldn't save the settings. Try again.");
        state_ = State::Saved;
        savedAt_ = lastActivity_ = nowMs;
        status_ = "Saved.";
    }

    Response currentPage() const {
        Response r;
        if (state_ == State::Form) {
            r.body = renderPortalPage(pending_, networks_, error_, status_.c_str());
            return r;
        }
        const bool saved = state_ == State::Saved;
        r.body = std::string("<!doctype html><html><head><meta charset=utf-8>")
            + (saved ? "" : "<meta http-equiv=refresh content=2>")
            + "<meta name=viewport content=\"width=device-width,initial-scale=1\"><title>Ventanal setup</title>"
              "<style>body{font:18px system-ui,sans-serif;margin:2em auto;max-width:30em;padding:1em;"
              "background:#000;color:#fc9}h1{color:#f93}</style></head><body><h1>Ventanal setup</h1><p>"
            + (saved ? "Saved. Ventanal is restarting with the new settings; you can close this page."
                     : "Checking the Wi-Fi network and your location\xe2\x80\xa6 This page refreshes by itself. "
                       "If your phone leaves the Ventanal network for a moment, rejoin it.")
            + "</p></body></html>";
        return r;
    }

    PortalHooks hooks_;
    std::vector<std::string> networks_;
    uint32_t lastActivity_;
    uint32_t savedAt_ = 0;
    State state_ = State::Form;
    PortalFields pending_{};
    FormError error_ = FormError::None;
    std::string status_;
    bool cancelled_ = false;
};

#ifndef VENTANAL_HOST_TEST
// The board side (portal.cpp): SoftAP + DNS + web server around PortalCore.
class SetupPortal {
public:
    // Scans for networks, then starts the WPA2 SoftAP (apPass: 8+ characters).
    bool begin(const char* apSsid, const char* apPass, PortalHooks hooks);
    PortalOutcome poll(uint32_t nowMs);
    void cancel();
    void end();  // stops serving and takes the SoftAP down
    const char* status() const;
};

// A `join` hook for boards: joins as a station while the SoftAP stays up,
// serving the portal and feeding the watchdog while it waits.
bool joinStation(const char* ssid, const char* password, uint32_t timeoutMs);
#endif

}  // namespace portal
