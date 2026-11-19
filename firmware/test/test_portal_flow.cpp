// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../common/setup_portal/portal.h"
#include "fakes/portal_net.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace portal;

static bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

static const char* goodBody = "ssid=Synthetic+Net&password=synthetic-pass&zip=12345&units=imperial&clock=24"
                              "&quiet_start=23%3A00&quiet_end=06%3A00&call=K1ABC";

// One poll lets the board draw "Checking" before the slow join; the next runs it.
static PortalOutcome settle(PortalCore& core, uint32_t nowMs) {
    core.poll(nowMs - 1);
    return core.poll(nowMs);
}

static PortalCore freshCore(uint32_t now = 1000) {
    fakenet::reset();
    return PortalCore(fakenet::hooks(), {"Synthetic Net", "Neighbour"}, now);
}

static void captive_probes_redirect_to_the_page() {
    PortalCore core = freshCore();
    const Response page = core.handleGet("/", 1000);
    assert(page.code == 200 && contains(page.body, "<form method=post action=/save>"));
    for (const char* probe : {"/generate_204", "/hotspot-detect.html", "/connecttest.txt", "/anything"}) {
        const Response r = core.handleGet(probe, 1000);
        assert(r.code == 302 && r.location == "http://192.168.4.1/");
    }
}

static void portal_saves_after_join_and_locate() {
    PortalCore core = freshCore();
    const Response accepted = core.handlePost("/save", goodBody, 2000);
    assert(accepted.code == 200 && contains(accepted.body, "Checking"));
    assert(contains(accepted.body, "http-equiv=refresh"));
    assert(fakenet::joins == 0);  // the slow work happens in poll, not in the request
    assert(core.poll(2050) == PortalOutcome::Running && fakenet::joins == 0);
    assert(contains(core.status(), "Checking") && !core.statusIsFailure());  // drawn before the join blocks
    assert(core.poll(2100) == PortalOutcome::Running);
    assert(fakenet::joins == 1 && !strcmp(fakenet::lastSsid, "Synthetic Net"));
    assert(!strcmp(fakenet::lastPassword, "synthetic-pass") && fakenet::lastTimeoutMs == 20000);
    assert(fakenet::locates == 1 && !strcmp(fakenet::lastZip, "12345"));
    assert(fakenet::saves == 1);
    const Response done = core.handleGet("/", 2200);
    assert(contains(done.body, "Saved") && !contains(done.body, "<form"));
    // The success page gets a moment to reach the phone before the board restarts.
    assert(core.poll(2200) == PortalOutcome::Running);
    assert(core.poll(2100 + 3000) == PortalOutcome::Saved);
    assert(fakenet::feeds > 0);
}

static void portal_rejects_failed_join_keeps_ap() {
    PortalCore core = freshCore();
    fakenet::joinResult = false;
    core.handlePost("/save", goodBody, 2000);
    assert(settle(core, 2100) == PortalOutcome::Running);
    assert(fakenet::joins == 1 && fakenet::locates == 0 && fakenet::saves == 0);
    const Response again = core.handleGet("/", 2200);
    assert(contains(again.body, "<form") && contains(again.body, "Couldn&#39;t join"));
    assert(contains(again.body, "<option value=\"Synthetic Net\" selected>"));
    assert(contains(again.body, "value=\"12345\"") && contains(again.body, "value=\"K1ABC\""));
    assert(!contains(again.body, "synthetic-pass"));
    assert(contains(core.status(), "Couldn't join") && core.statusIsFailure());
    // A corrected second attempt succeeds.
    fakenet::joinResult = true;
    core.handlePost("/save", goodBody, 3000);
    settle(core, 3100);
    assert(fakenet::joins == 2 && fakenet::saves == 1);
}

static void unknown_zip_is_reported() {
    PortalCore core = freshCore();
    fakenet::location.why = LocationResult::ZipUnknown;
    fakenet::location.ok = false;
    core.handlePost("/save", goodBody, 2000);
    settle(core, 2100);
    assert(fakenet::saves == 0);
    const Response page = core.handleGet("/", 2200);
    assert(contains(page.body, formErrorText(FormError::Zip)) && contains(page.body, "ZIP not found"));
}

static void unreachable_locator_is_reported() {
    PortalCore core = freshCore();
    fakenet::location.why = LocationResult::Network;
    fakenet::location.ok = false;
    core.handlePost("/save", goodBody, 2000);
    settle(core, 2100);
    assert(fakenet::saves == 0 && contains(core.status(), "location service"));
}

static void save_is_single_putBytes() {
    PortalCore core = freshCore();
    core.handlePost("/save", goodBody, 2000);
    core.handlePost("/save", goodBody, 2050);  // a double-tap while checking is ignored
    settle(core, 2100);
    core.poll(2200);
    assert(fakenet::saves == 1);
    assert(!strcmp(fakenet::savedFields.ssid, "Synthetic Net") && !strcmp(fakenet::savedFields.zip, "12345"));
    assert(fakenet::savedFields.quietStart == 23 * 60 && !strcmp(fakenet::savedFields.callSign, "K1ABC"));
    assert(!strcmp(fakenet::savedLocation.posixTz, "EST5EDT,M3.2.0,M11.1.0"));
    fakenet::reset();
    PortalCore failing(fakenet::hooks(), {}, 0);
    fakenet::saveResult = false;
    failing.handlePost("/save", goodBody, 10);
    assert(settle(failing, 20) == PortalOutcome::Running && contains(failing.status(), "Couldn't save"));
}

static void host_join_without_zip_skips_locate() {
    PortalCore core = freshCore();
    const std::string body = std::string("ssid=n&password=synthetic-pass&host=https%3A%2F%2Fframe.test")
        + "&pairing=desk&certificate=-----BEGIN+CERTIFICATE-----x&device_key=" + std::string(64, 'a');
    core.handlePost("/save", body.c_str(), 2000);
    settle(core, 2100);
    assert(fakenet::locates == 0 && fakenet::saves == 1 && !fakenet::savedLocation.ok);
}

static void form_errors_answer_immediately() {
    PortalCore core = freshCore();
    const Response r = core.handlePost("/save", "ssid=n&zip=1234", 2000);
    assert(r.code == 200 && contains(r.body, formErrorText(FormError::Zip)));
    settle(core, 2100);
    assert(fakenet::joins == 0);
}

static void portal_times_out_after_15_min() {
    PortalCore core = freshCore(1000);
    assert(core.poll(1000 + 15 * 60 * 1000 - 1) == PortalOutcome::Running);
    core.handleGet("/", 1000 + 10 * 60 * 1000);  // activity restarts the idle clock
    assert(core.poll(1000 + 24 * 60 * 1000) == PortalOutcome::Running);
    assert(core.poll(1000 + 25 * 60 * 1000) == PortalOutcome::TimedOut);
    PortalCore wrap = freshCore(0xffffff00u);     // millis() wraps after ~49 days
    assert(wrap.poll(0x100) == PortalOutcome::Running);
    // A caller's timestamp taken before the portal started (the network scan
    // runs in between) is not 49 days of idling.
    PortalCore stale = freshCore(50000);
    assert(stale.poll(46000) == PortalOutcome::Running);
    stale.handleGet("/", 60000);
    assert(stale.poll(59000) == PortalOutcome::Running);
}

static void cancel_ends_the_portal() {
    PortalCore core = freshCore();
    core.cancel();
    assert(core.poll(1001) == PortalOutcome::Cancelled);
}

int main() {
    captive_probes_redirect_to_the_page();
    portal_saves_after_join_and_locate();
    portal_rejects_failed_join_keeps_ap();
    unknown_zip_is_reported();
    unreachable_locator_is_reported();
    save_is_single_putBytes();
    host_join_without_zip_skips_locate();
    form_errors_answer_immediately();
    portal_times_out_after_15_min();
    cancel_ends_the_portal();
    puts("portal flow: captive redirect, join-before-save, errors, single save, idle timeout passed");
}
