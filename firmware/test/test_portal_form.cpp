// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../common/setup_portal/form.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace portal;

static bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

static const char* standaloneBody =
    "ssid=Synthetic+Net&password=synthetic-pass&zip=12345&units=imperial&clock=24"
    "&quiet_start=23%3A00&quiet_end=06%3A30&call=+k1abc+";

static void ap_name_uses_last_two_mac_bytes() {
    const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0xaa, 0x3c, 0x0f};
    assert(apName(mac) == "Ventanal-3C0F");
}

static uint32_t lcg() {
    static uint32_t state = 12345;
    state = state * 1664525u + 1013904223u;
    return state;
}

static uint32_t biased() {  // the top of the range must be rejected, never folded
    static int calls = 0;
    return calls++ % 2 ? 0u : 0xffffffffu;
}

static void ap_password_alphabet_and_length() {
    char pw[11];
    for (int i = 0; i < 10000; ++i) {
        apPassword(lcg, pw);
        assert(strlen(pw) == 10);
        for (char c : std::string(pw)) {
            assert(strchr(apPasswordAlphabet, c));
            assert(!strchr("0O1Il", c));
        }
    }
    apPassword(biased, pw);
    assert(!strcmp(pw, "2222222222"));  // every 0xffffffff draw was rejected
}

static void form_round_trip() {
    PortalFields f{};
    assert(parsePortalForm(standaloneBody, f) == FormError::None);
    assert(!strcmp(f.ssid, "Synthetic Net") && !strcmp(f.password, "synthetic-pass"));
    assert(!strcmp(f.zip, "12345") && f.units == 0 && f.clock24h == 1);
    assert(f.quietStart == 23 * 60 && f.quietEnd == 6 * 60 + 30);
    assert(!strcmp(f.callSign, "K1ABC"));
    assert(!f.host[0] && !f.pairing[0] && !f.certificate[0] && !f.deviceKey[0]);

    // %2B is a literal plus, + is a space, UTF-8 survives.
    assert(parsePortalForm("ssid=a%2Bb+c%C3%A9&password=&zip=12345-6789&units=metric&clock=12"
                           "&quiet_start=&quiet_end=", f) == FormError::None);
    assert(!strcmp(f.ssid, "a+b c\xc3\xa9") && !f.password[0]);
    assert(!strcmp(f.zip, "12345-6789") && f.units == 1 && f.clock24h == 0);
    assert(f.quietStart == -1 && f.quietEnd == -1 && !f.callSign[0]);

    // "Other network" text wins when the list selection is empty.
    assert(parsePortalForm("ssid=&ssid_other=Hidden+Net&zip=12345", f) == FormError::None);
    assert(!strcmp(f.ssid, "Hidden Net"));
}

static void form_rejects_bad_fields() {
    PortalFields f{};
    assert(parsePortalForm("ssid=n&zip=1234", f) == FormError::Zip);
    assert(parsePortalForm("ssid=n&zip=ABCDE", f) == FormError::Zip);
    assert(parsePortalForm("ssid=n", f) == FormError::Zip);  // standalone needs a ZIP
    assert(parsePortalForm("zip=12345", f) == FormError::Ssid);
    assert(parsePortalForm(("ssid=" + std::string(33, 'n') + "&zip=12345").c_str(), f) == FormError::Ssid);
    assert(parsePortalForm("ssid=n&password=short&zip=12345", f) == FormError::Password);
    assert(parsePortalForm(("ssid=n&zip=12345&password=" + std::string(65, 'p')).c_str(), f)
           == FormError::Password);
    assert(parsePortalForm("ssid=n&zip=12345&quiet_start=23%3A00&quiet_end=", f) == FormError::Quiet);
    assert(parsePortalForm("ssid=n&zip=12345&quiet_start=24%3A00&quiet_end=06%3A00", f) == FormError::Quiet);
    assert(parsePortalForm("ssid=n&zip=12345&call=K1+ABC", f) == FormError::CallSign);
    assert(parsePortalForm("ssid=n&zip=12345&call=K1", f) == FormError::CallSign);
    assert(parsePortalForm("ssid=n&zip=12345&units=kelvin", f) == FormError::Malformed);
    assert(parsePortalForm("ssid=n%zz&zip=12345", f) == FormError::Malformed);
}

static std::string hostBody(const char* host, const char* pairing, const char* cert, const char* key) {
    return std::string("ssid=n&host=") + host + "&pairing=" + pairing + "&certificate=" + cert
        + "&device_key=" + key;
}

static void form_partial_host_rejected() {
    const std::string key(64, 'A');
    const char* pem = "-----BEGIN+CERTIFICATE-----%0D%0Aabc%0D%0A-----END+CERTIFICATE-----";
    PortalFields f{};
    assert(parsePortalForm(hostBody("https%3A%2F%2Fframe.test", "", "", "").c_str(), f) == FormError::Host);
    assert(parsePortalForm(hostBody("https%3A%2F%2Fframe.test", "desk", pem, "").c_str(), f) == FormError::Host);
    assert(parsePortalForm(hostBody("http%3A%2F%2Fframe.test", "desk", pem, key.c_str()).c_str(), f)
           == FormError::Host);  // a paired host is HTTPS only
    assert(parsePortalForm(hostBody("https%3A%2F%2Fframe.test", "Desk!", pem, key.c_str()).c_str(), f)
           == FormError::Pairing);
    assert(parsePortalForm(hostBody("https%3A%2F%2Fframe.test", "desk", "not+a+pem", key.c_str()).c_str(), f)
           == FormError::Host);
    // Complete: no ZIP needed with a host; CRLF becomes LF; the key is lower-cased.
    assert(parsePortalForm(hostBody("https%3A%2F%2Fframe.test%2F", "desk", pem, key.c_str()).c_str(), f)
           == FormError::None);
    assert(!strcmp(f.host, "https://frame.test/") && !strcmp(f.pairing, "desk"));
    assert(!strcmp(f.certificate, "-----BEGIN CERTIFICATE-----\nabc\n-----END CERTIFICATE-----"));
    assert(f.deviceKey == std::string(64, 'a'));
}

static void qr_escapes_specials() {
    assert(wifiQrPayload("Ventanal-3C0F", "abcDEF2345") == "WIFI:T:WPA;S:Ventanal-3C0F;P:abcDEF2345;;");
    assert(wifiQrPayload("a;b,c:d\\e\"f", "p;w") == "WIFI:T:WPA;S:a\\;b\\,c\\:d\\\\e\\\"f;P:p\\;w;;");
}

static void page_escapes_scanned_ssid() {
    PortalFields prefill{};
    const std::string page = renderPortalPage(prefill, {"<script>x</script>", "Cafe \"Wi-Fi\" & more"},
                                              FormError::None, "");
    assert(!contains(page, "<script>"));
    assert(contains(page, "&lt;script&gt;x&lt;/script&gt;"));
    assert(contains(page, "Cafe &quot;Wi-Fi&quot; &amp; more"));
}

static void page_prefills_and_reports() {
    PortalFields prefill{};
    assert(parsePortalForm(standaloneBody, prefill) == FormError::None);
    const std::string page = renderPortalPage(prefill, {"Other Net", "Synthetic Net"},
                                              FormError::Password, "Couldn't join <net>");
    assert(contains(page, "<option value=\"Synthetic Net\" selected>"));
    assert(contains(page, "value=\"12345\"") && contains(page, "value=\"K1ABC\""));
    assert(contains(page, "value=\"23:00\"") && contains(page, "value=\"06:30\""));
    assert(contains(page, "Couldn&#39;t join &lt;net&gt;"));
    assert(contains(page, formErrorText(FormError::Password)));
}

static void page_never_echoes_secrets() {
    PortalFields prefill{};
    strcpy(prefill.ssid, "n");
    strcpy(prefill.password, "hunter2-secret");
    strcpy(prefill.certificate, "-----BEGIN CERTIFICATE-----SECRETPEM");
    memset(prefill.deviceKey, 'f', 64);
    strcpy(prefill.host, "https://frame.test");
    strcpy(prefill.pairing, "desk");
    const std::string page = renderPortalPage(prefill, {}, FormError::None, "");
    assert(!contains(page, "hunter2-secret") && !contains(page, "SECRETPEM"));
    assert(!contains(page, std::string(64, 'f').c_str()));
    assert(contains(page, "https://frame.test"));  // the host itself is not secret
}

// What the board is set up with now: the portal starts from it.
static PortalFields currentPaired(const char* host = "https://frame.test") {
    PortalFields c{};
    strcpy(c.ssid, "Synthetic Net");
    strcpy(c.password, "synthetic-pass");
    strcpy(c.zip, "12345");
    strcpy(c.host, host);
    strcpy(c.pairing, "desk");
    strcpy(c.certificate, "-----BEGIN CERTIFICATE-----current");
    memset(c.deviceKey, 'c', 64);
    return c;
}

static void unchanged_host_needs_no_secrets() {
    const PortalFields current = currentPaired();
    PortalFields f{};
    // Host and pairing as shown, certificate and key left blank: keep the pairing.
    assert(parsePortalForm("ssid=Synthetic+Net&zip=12345&host=https%3A%2F%2Fframe.test&pairing=desk", f, &current)
           == FormError::None);
    assert(!strcmp(f.host, "https://frame.test") && !strcmp(f.pairing, "desk"));
    assert(!strcmp(f.certificate, current.certificate) && !strcmp(f.deviceKey, current.deviceKey));
    // Host fields cleared without ticking Leave: still the current host.
    assert(parsePortalForm("ssid=Synthetic+Net&zip=12345", f, &current) == FormError::None);
    assert(!strcmp(f.host, "https://frame.test") && !f.leaveHost);
    // The bench's plain-HTTP host is kept as it is (only a new pairing must be HTTPS).
    PortalFields bench = currentPaired("http://bench.test");
    bench.pairing[0] = bench.certificate[0] = bench.deviceKey[0] = '\0';
    assert(parsePortalForm("ssid=Synthetic+Net&host=http%3A%2F%2Fbench.test", f, &bench) == FormError::None);
    assert(!strcmp(f.host, "http://bench.test") && !f.zip[0]);
    // A different host is a new pairing and needs everything.
    assert(parsePortalForm("ssid=n&zip=12345&host=https%3A%2F%2Fother.test&pairing=desk", f, &current)
           == FormError::Host);
}

static void leave_host_clears_the_pairing() {
    const PortalFields current = currentPaired();
    PortalFields f{};
    assert(parsePortalForm("ssid=Synthetic+Net&zip=12345&leave_host=1&host=https%3A%2F%2Fframe.test&pairing=desk",
                           f, &current) == FormError::None);
    assert(f.leaveHost && !f.host[0] && !f.pairing[0] && !f.certificate[0] && !f.deviceKey[0]);
    assert(parsePortalForm("ssid=Synthetic+Net&leave_host=1", f, &current) == FormError::Zip);  // standalone needs a ZIP
}

static void password_kept_for_the_same_network() {
    const PortalFields current = currentPaired();
    PortalFields f{};
    assert(parsePortalForm("ssid=Synthetic+Net&password=&zip=12345", f, &current) == FormError::None);
    assert(!strcmp(f.password, "synthetic-pass"));
    assert(parsePortalForm("ssid=Other+Net&password=&zip=12345", f, &current) == FormError::None);
    assert(!f.password[0]);  // another network with no password: an open network
    assert(parsePortalForm("ssid=Synthetic+Net&password=new-password&zip=12345", f, &current) == FormError::None);
    assert(!strcmp(f.password, "new-password"));
}

static void page_offers_leave_only_with_a_host() {
    const PortalFields current = currentPaired();
    const std::string paired = renderPortalPage(current, {}, FormError::None, "");
    assert(contains(paired, "name=leave_host") && contains(paired, "<details open>"));
    assert(contains(paired, "placeholder=\"unchanged\""));
    assert(!contains(paired, "synthetic-pass") && !contains(paired, "current") && !contains(paired, std::string(64, 'c').c_str()));
    PortalFields alone{};
    strcpy(alone.ssid, "n");
    assert(!contains(renderPortalPage(alone, {}, FormError::None, ""), "name=leave_host"));
}

static void page_stays_small() {
    std::vector<std::string> nets;
    for (int i = 0; i < 20; ++i) nets.push_back(std::string(32, char('a' + i)));
    PortalFields prefill{};
    assert(renderPortalPage(prefill, nets, FormError::Quiet, "status").size() < 8192);
}

int main() {
    ap_name_uses_last_two_mac_bytes();
    ap_password_alphabet_and_length();
    form_round_trip();
    form_rejects_bad_fields();
    form_partial_host_rejected();
    qr_escapes_specials();
    page_escapes_scanned_ssid();
    page_prefills_and_reports();
    page_never_echoes_secrets();
    page_stays_small();
    unchanged_host_needs_no_secrets();
    leave_host_clears_the_pairing();
    password_kept_for_the_same_network();
    page_offers_leave_only_with_a_host();
    puts("portal form: AP credentials, parsing, host pairing, QR and page escaping passed");
}
