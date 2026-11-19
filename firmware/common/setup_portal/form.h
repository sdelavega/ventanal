// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// The setup portal's form model: parse and validate what the phone posts,
// name and password the setup hotspot, and render the one page it serves.
// Pure C++ (no Arduino headers) so both boards and the host tests share it.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace portal {

struct PortalFields {
    char ssid[33]{};
    char password[65]{};
    char zip[11]{};
    uint8_t units = 0;       // 0 imperial, 1 metric
    uint8_t clock24h = 1;
    int16_t quietStart = -1; // minutes of the local day; both -1 = off
    int16_t quietEnd = -1;
    char callSign[11]{};
    char host[256]{};
    char pairing[33]{};
    char certificate[4096]{};
    char deviceKey[65]{};
    uint8_t leaveHost = 0;   // the "Leave the host" box was ticked
};

enum class FormError { None, Ssid, Password, Zip, Quiet, CallSign, Host, Pairing, Malformed };

inline const char* formErrorText(FormError e) {
    switch (e) {
    case FormError::None: return "";
    case FormError::Ssid: return "Choose a Wi-Fi network (up to 32 characters).";
    case FormError::Password: return "Wi-Fi passwords are empty (open network) or 8 to 64 characters.";
    case FormError::Zip: return "Enter a five-digit US ZIP code (or ZIP+4).";
    case FormError::Quiet: return "Quiet hours need both a start and an end time, or neither.";
    case FormError::CallSign: return "Call signs are 3 to 10 letters and digits, or leave it empty.";
    case FormError::Host: return "A host needs an https:// address, its pairing name, certificate and device key.";
    case FormError::Pairing: return "The pairing name uses lowercase letters, digits and dashes.";
    case FormError::Malformed: return "The form could not be read. Please try again.";
    }
    return "";
}

// ---- hotspot credentials -------------------------------------------------

inline std::string apName(const uint8_t mac[6]) {
    char name[16];
    snprintf(name, sizeof name, "Ventanal-%02X%02X", mac[4], mac[5]);
    return name;
}

// No 0/O, 1/I/l (or o): the password is read off the board's screen.
inline constexpr char apPasswordAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz";

inline void apPassword(uint32_t (*rand32)(), char out[11]) {
    constexpr uint64_t n = sizeof apPasswordAlphabet - 1;
    constexpr uint64_t limit = (uint64_t(1) << 32) - (uint64_t(1) << 32) % n;  // reject the biased tail
    for (int i = 0; i < 10;) {
        const uint32_t r = rand32();
        if (r < limit) out[i++] = apPasswordAlphabet[r % n];
    }
    out[10] = '\0';
}

// The Wi-Fi QR format phones understand ("WIFI:T:WPA;S:...;P:...;;").
inline std::string wifiQrPayload(const char* ssid, const char* password) {
    auto escaped = [](const char* s) {
        std::string out;
        for (; *s; ++s) {
            if (strchr("\\;,:\"", *s)) out += '\\';
            out += *s;
        }
        return out;
    };
    return "WIFI:T:WPA;S:" + escaped(ssid) + ";P:" + escaped(password) + ";;";
}

// ---- parsing --------------------------------------------------------------

namespace detail {

inline int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool urlDecode(const char* begin, const char* end, std::string& out) {
    out.clear();
    for (const char* p = begin; p < end; ++p) {
        if (*p == '+') {
            out += ' ';
        } else if (*p == '%') {
            if (end - p < 3) return false;
            const int hi = hexValue(p[1]), lo = hexValue(p[2]);
            if (hi < 0 || lo < 0) return false;
            out += char(hi * 16 + lo);
            p += 2;
        } else {
            out += *p;
        }
    }
    return true;
}

inline std::string trimmed(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

template<size_t N>
inline bool copyField(char (&dst)[N], const std::string& value) {
    if (value.size() >= N || value.find('\0') != std::string::npos) return false;
    memcpy(dst, value.c_str(), value.size() + 1);
    return true;
}

// "HH:MM" -> minutes; "" -> -1; anything else -> -2.
inline int clockMinutes(const std::string& v) {
    if (v.empty()) return -1;
    if (v.size() != 5 || v[2] != ':' || !isdigit((unsigned char)v[0]) || !isdigit((unsigned char)v[1])
        || !isdigit((unsigned char)v[3]) || !isdigit((unsigned char)v[4]))
        return -2;
    const int h = (v[0] - '0') * 10 + (v[1] - '0'), m = (v[3] - '0') * 10 + (v[4] - '0');
    return h < 24 && m < 60 ? h * 60 + m : -2;
}

inline bool zipValid(const std::string& z) {
    auto digits = [&](size_t from, size_t n) {
        for (size_t i = from; i < from + n; ++i)
            if (!isdigit((unsigned char)z[i])) return false;
        return true;
    };
    return (z.size() == 5 && digits(0, 5)) || (z.size() == 10 && digits(0, 5) && z[5] == '-' && digits(6, 4));
}

inline bool slug(const std::string& s) {
    return !s.empty() && s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos;
}

// https://authority[/...] with nothing that could smuggle credentials or a query.
inline bool httpsHost(const std::string& host) {
    if (host.compare(0, 8, "https://")) return false;
    const std::string rest = host.substr(8);
    const size_t slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    if (authority.empty() || authority.find_first_of("?#@ \t\r\n") != std::string::npos) return false;
    return slash == std::string::npos || rest.find_first_not_of('/', slash) == std::string::npos;
}

}  // namespace detail

// `current` is what the board is set up with now (null on a first setup).
// Blank secrets mean "unchanged": the Wi-Fi password when the network is the
// same one, and the host pairing when the host and pairing name are as shown
// (or cleared) and no new certificate or key was given. A host is dropped
// only when "Leave the host" is ticked.
inline FormError parsePortalForm(const char* body, PortalFields& out, const PortalFields* current = nullptr) {
    using namespace detail;
    out = PortalFields{};
    std::string ssid, ssidOther, password, zip, units = "imperial", clock = "24";
    std::string quietStart, quietEnd, call, host, pairing, certificate, deviceKey, leave;
    struct { const char* name; std::string* value; } fields[] = {
        {"ssid", &ssid}, {"ssid_other", &ssidOther}, {"password", &password}, {"zip", &zip},
        {"units", &units}, {"clock", &clock}, {"quiet_start", &quietStart}, {"quiet_end", &quietEnd},
        {"call", &call}, {"host", &host}, {"pairing", &pairing}, {"certificate", &certificate},
        {"device_key", &deviceKey}, {"leave_host", &leave},
    };
    for (const char* p = body; *p;) {
        const char* end = p + strcspn(p, "&");
        const char* eq = static_cast<const char*>(memchr(p, '=', end - p));
        std::string key, value;
        if (!eq || !urlDecode(p, eq, key) || !urlDecode(eq + 1, end, value)) return FormError::Malformed;
        for (auto& f : fields)
            if (key == f.name) *f.value = value;  // unknown names are ignored
        p = *end ? end + 1 : end;
    }

    if (ssid.empty()) ssid = ssidOther;
    if (ssid.empty() || !copyField(out.ssid, ssid)) return FormError::Ssid;
    if (password.empty() && current && ssid == current->ssid) password = current->password;
    if ((!password.empty() && password.size() < 8) || !copyField(out.password, password))
        return FormError::Password;
    if (units != "imperial" && units != "metric") return FormError::Malformed;
    if (clock != "12" && clock != "24") return FormError::Malformed;
    out.units = units == "metric";
    out.clock24h = clock == "24";

    const int qs = clockMinutes(trimmed(quietStart)), qe = clockMinutes(trimmed(quietEnd));
    if (qs == -2 || qe == -2 || (qs == -1) != (qe == -1)) return FormError::Quiet;
    out.quietStart = int16_t(qs);
    out.quietEnd = int16_t(qe);

    call = trimmed(call);
    for (char& c : call) c = char(toupper((unsigned char)c));
    if (!call.empty() && (call.size() < 3 || call.size() > 10
                          || call.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos))
        return FormError::CallSign;
    copyField(out.callSign, call);

    host = trimmed(host);
    pairing = trimmed(pairing);
    deviceKey = trimmed(deviceKey);
    for (char& c : deviceKey) c = char(tolower((unsigned char)c));
    std::string pem;
    for (char c : certificate)
        if (c != '\r') pem += c;
    pem = trimmed(pem);
    const bool anyHost = !host.empty() || !pairing.empty() || !pem.empty() || !deviceKey.empty();
    const bool keepHost = current && current->host[0] && pem.empty() && deviceKey.empty()
        && (host.empty() || host == current->host) && (pairing.empty() || pairing == current->pairing);
    if (!leave.empty() && leave != "0") {
        out.leaveHost = 1;  // host fields are ignored: the board will run on its own
    } else if (keepHost) {
        copyField(out.host, current->host);  // as stored, including a bench plain-HTTP host
        copyField(out.pairing, current->pairing);
        copyField(out.certificate, current->certificate);
        copyField(out.deviceKey, current->deviceKey);
    } else if (anyHost) {
        if (!httpsHost(host) || !copyField(out.host, host)) return FormError::Host;
        if (pem.find("-----BEGIN CERTIFICATE-----") == std::string::npos || !copyField(out.certificate, pem))
            return FormError::Host;
        if (deviceKey.size() != 64 || deviceKey.find_first_not_of("0123456789abcdef") != std::string::npos)
            return FormError::Host;
        copyField(out.deviceKey, deviceKey);
        if (pairing.empty()) return FormError::Host;
        if (!slug(pairing) || !copyField(out.pairing, pairing)) return FormError::Pairing;
    }

    zip = trimmed(zip);
    if (zip.empty() ? !out.host[0] : !zipValid(zip)) return FormError::Zip;  // standalone needs a ZIP
    copyField(out.zip, zip);
    return FormError::None;
}

// ---- the page -------------------------------------------------------------

inline std::string htmlEscape(const char* s) {
    std::string out;
    for (; *s; ++s) {
        switch (*s) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += *s;
        }
    }
    return out;
}

// The Wi-Fi password, certificate and device key are never written back into
// the page: a failed submit asks for them again.
inline std::string renderPortalPage(const PortalFields& prefill, const std::vector<std::string>& networks,
                                    FormError error, const char* status, int offerLeave = -1) {
    const bool leaveOffered = offerLeave < 0 ? prefill.host[0] != 0 : offerLeave != 0;
    const char* unchanged = " placeholder=\"unchanged\"";
    auto clockText = [](int16_t m) {
        if (m < 0 || m >= 24 * 60) return std::string();
        const char t[] = {char('0' + m / 600), char('0' + m / 60 % 10), ':',
                          char('0' + m % 60 / 10), char('0' + m % 10), '\0'};
        return std::string(t);
    };
    auto option = [](const std::string& value, const char* label, bool selected) {
        return "<option value=\"" + htmlEscape(value.c_str()) + "\"" + (selected ? " selected>" : ">")
            + htmlEscape(label) + "</option>";
    };
    bool listed = false;
    std::string nets;
    for (const auto& n : networks) {
        const bool sel = prefill.ssid[0] && n == prefill.ssid;
        listed = listed || sel;
        nets += option(n, n.c_str(), sel);
    }
    nets += option("", "Other network\xe2\x80\xa6", !listed && networks.size());

    std::string page =
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\"><title>Ventanal setup</title>"
        "<style>body{font:16px system-ui,sans-serif;margin:0 auto;max-width:30em;padding:1em;background:#000;"
        "color:#fc9}h1{color:#f93}label{display:block;margin:.8em 0 .2em}input,select,textarea{width:100%;"
        "box-sizing:border-box;font:inherit;padding:.4em;border-radius:.4em;border:0}button{margin-top:1.2em;"
        "width:100%;padding:.7em;font:inherit;background:#f93;border:0;border-radius:2em}.err{background:#c44;"
        "color:#fff;padding:.6em;border-radius:.4em}.st{color:#9cf}.row{display:flex;gap:.6em}"
        ".row>*{flex:1}</style></head><body><h1>Ventanal setup</h1>";
    if (error != FormError::None)
        page += std::string("<p class=err>") + formErrorText(error) + "</p>";
    if (status && *status) page += "<p class=st>" + htmlEscape(status) + "</p>";
    page += "<form method=post action=/save>";
    if (!networks.empty()) page += "<label>Wi-Fi network</label><select name=ssid>" + nets + "</select>";
    page += std::string("<label>") + (networks.empty() ? "Wi-Fi network" : "Other network (if not listed)")
        + "</label><input name=ssid_other maxlength=32 value=\""
        + htmlEscape(listed ? "" : prefill.ssid) + "\">"
        "<label>Wi-Fi password</label><input name=password type=password maxlength=64 autocomplete=off"
        + (prefill.password[0] ? unchanged : "") + ">"
        "<label>ZIP code</label><input name=zip inputmode=numeric maxlength=10 value=\""
        + htmlEscape(prefill.zip) + "\">"
        "<div class=row><div><label>Units</label><select name=units>"
        + option("imperial", "\xc2\xb0" "F, mph", prefill.units == 0) + option("metric", "\xc2\xb0" "C, km/h", prefill.units == 1)
        + "</select></div><div><label>Clock</label><select name=clock>"
        + option("12", "12-hour", !prefill.clock24h) + option("24", "24-hour", prefill.clock24h)
        + "</select></div></div>"
        "<div class=row><div><label>Quiet from</label><input name=quiet_start type=time value=\""
        + clockText(prefill.quietStart) + "\"></div><div><label>Quiet until</label>"
        "<input name=quiet_end type=time value=\"" + clockText(prefill.quietEnd) + "\"></div></div>"
        "<label>Amateur radio call sign (optional, for the solar screen)</label>"
        "<input name=call maxlength=10 autocapitalize=characters value=\"" + htmlEscape(prefill.callSign) + "\">"
        "<details" + (prefill.host[0] || leaveOffered ? " open" : "") + "><summary>Use a Ventanal host</summary>"
        + (leaveOffered ? std::string("<label><input type=checkbox name=leave_host value=1")
                              + (prefill.leaveHost ? " checked" : "")
                              + " style=width:auto> Leave the host and run on its own</label>"
                        : std::string())
        + "<label>Host address</label><input name=host type=url placeholder=\"https://\" value=\""
        + htmlEscape(prefill.host) + "\">"
        "<label>Pairing name</label><input name=pairing maxlength=32 value=\"" + htmlEscape(prefill.pairing) + "\">"
        "<label>Host certificate (PEM)</label><textarea name=certificate rows=4"
        + (prefill.certificate[0] ? unchanged : "") + "></textarea>"
        "<label>Device key</label><input name=device_key maxlength=64 autocomplete=off"
        + (prefill.deviceKey[0] ? unchanged : "") + "></details>"
        "<button>Save and connect</button></form></body></html>";
    return page;
}

}  // namespace portal
