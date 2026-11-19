// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
//
// Standalone space weather: the NOAA SWPC products the server reads
// (server/src/ventanal/solar/data.py fetch_solar, active_alerts,
// forecast_days), its HF estimate (solar/conditions.py) and the solar
// screen's helpers (solar/render.py). The server's regular expressions are
// small hand matchers here. Pure; host tested against the server's own
// results on the same fixtures (tools/standalone_expected.py).
#pragma once

#include "weather_data.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lcd::local {

struct KpReading {
    char timeTag[24]{};  // "2026-11-19 21:00:00.000"
    float kp = 0;
};

struct ScaleDay {
    char dateStamp[16]{};  // "2026-11-19"
    std::string rMinorProb, sProb, gScale;  // "" when the product has null
    bool hasR = false, hasS = false;
};

struct LocalSolar {
    float sfi = 0;
    int ssn = 0;
    char xray[12]{};       // "B4.9"
    float k = 0;
    int a = 0;
    KpReading kp[8];       // the last eight 3-hour readings, oldest first
    int kpCount = 0;
    float kpMax[3] = {};   // the 3-day forecast's daily Kp maxima
    ScaleDay scales[4];    // "0" (now) .. "3"
    std::string scaleNow[3];  // R, S, G scales now; "" for null
    std::vector<std::string> alerts;  // active_alerts(), sorted
};

namespace solar_detail {

inline bool startsWith(const std::string& s, const char* prefix) { return s.compare(0, strlen(prefix), prefix) == 0; }

inline std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
}

inline std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

inline bool isWord(char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; }

inline std::vector<std::string> lines(const char* text) {
    std::vector<std::string> out;
    std::string line;
    for (const char* p = text; *p; ++p) {
        if (*p == '\r') continue;
        if (*p == '\n') {
            out.push_back(line);
            line.clear();
        } else {
            line += *p;
        }
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

inline std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t j = i;
        while (j < s.size() && !isspace(static_cast<unsigned char>(s[j]))) ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

inline bool digitsAt(const std::string& s, size_t at, size_t n) {
    if (at + n > s.size()) return false;
    for (size_t i = at; i < at + n; ++i)
        if (!isdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

// Python float(): accepts the numeric strings these products use.
inline bool toFloat(const std::string& s, double& out) {
    char* end = nullptr;
    out = strtod(s.c_str(), &end);
    return end && end != s.c_str() && *end == '\0';
}

inline double numberOf(JsonVariantConst v) {
    if (v.is<double>()) return v.as<double>();
    double d = 0;
    const char* s = v.as<const char*>();
    return s && toFloat(s, d) ? d : 0;
}

inline std::string stringOf(JsonVariantConst v) {
    if (v.isNull()) return "";
    if (v.is<const char*>()) return v.as<const char*>();
    if (v.is<long>()) return std::to_string(v.as<long>());
    char text[32];
    snprintf(text, sizeof text, "%g", v.as<double>());
    return text;
}

static const char* const Months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

inline int monthIndex(const std::string& name) {  // strptime %b: case-insensitive
    for (int i = 0; i < 12; ++i)
        if (lower(name) == lower(Months[i])) return i + 1;
    return 0;
}

// "2026-11-14 22:49:51.473" (SWPC issue_datetime) -> epoch, UTC.
inline bool issueEpoch(const char* s, int64_t& out) {
    LocalClock c;
    std::string text = s ? s : "";
    if (text.size() >= 16 && text[10] == ' ') text[10] = 'T';
    if (!detail::parseClock(text.c_str(), c)) return false;
    int seconds = 0;
    if (text.size() >= 19 && text[16] == ':') detail::digits(text.c_str() + 17, 2, seconds);
    out = detail::clockMinutes(c) * 60 + seconds;
    return true;
}

inline std::string utcStamp(int64_t epoch, bool withYear = false) {  // strftime('%b %d %H%M UTC')
    const LocalClock c = [&] {
        LocalClock k;
        const int64_t days = (epoch >= 0 ? epoch : epoch - 86399) / 86400;
        const int64_t s = epoch - days * 86400;
        k.date = civilFromDays(days);
        k.hour = uint8_t(s / 3600);
        k.minute = uint8_t(s / 60 % 60);
        return k;
    }();
    char text[32];
    if (withYear)
        snprintf(text, sizeof text, "%02d %s %04d %02d:%02d", c.date.day, Months[c.date.month - 1], c.date.year,
                 c.hour, c.minute);
    else
        snprintf(text, sizeof text, "%s %02d %02d%02d UTC", Months[c.date.month - 1], c.date.day, c.hour, c.minute);
    return text;
}

}  // namespace solar_detail

// ---- products ---------------------------------------------------------------

// daily-solar-indices.txt: the newest row with a flux and a sunspot number.
inline bool parseDailyIndices(const char* text, float& sfi, int& ssn) {
    using namespace solar_detail;
    std::vector<std::string> best;
    for (const std::string& line : lines(text ? text : "")) {
        if (!(digitsAt(line, 0, 4) && line.size() > 4 && isspace(static_cast<unsigned char>(line[4]))))
            continue;
        std::vector<std::string> r = split(line);  // ^\d{4}\s+\d{2}\s+\d{2}
        if (r.size() < 5 || r[1].size() != 2 || r[2].size() != 2 || !digitsAt(r[1], 0, 2) || !digitsAt(r[2], 0, 2))
            continue;
        double flux = 0, spots = 0;
        if (!toFloat(r[3], flux) || !toFloat(r[4], spots) || flux <= 0 || spots < 0) continue;
        const std::vector<std::string> key(r.begin(), r.begin() + 3);
        if (best.empty() || key > std::vector<std::string>(best.begin(), best.begin() + 3)) best = r;
    }
    if (best.empty()) return false;
    double flux = 0;
    toFloat(best[3], flux);
    sfi = float(flux);
    ssn = atoi(best[4].c_str());
    return true;
}

// noaa-planetary-k-index.json: dict rows (or a header row and lists).
inline bool parseKp(const char* json, LocalSolar& s) {
    JsonDocument doc;
    if (!json || deserializeJson(doc, json) || !doc.is<JsonArrayConst>()) return false;
    JsonArrayConst rows = doc.as<JsonArrayConst>();
    std::vector<std::pair<std::string, std::pair<double, int>>> readings;
    if (rows.size() && rows[0].is<JsonArrayConst>()) {
        JsonArrayConst header = rows[0];
        int tag = -1, kp = -1, a = -1, i = 0;
        for (JsonVariantConst h : header) {
            const std::string name = h | "";
            if (name == "time_tag") tag = i;
            if (name == "Kp") kp = i;
            if (name == "a_running") a = i;
            ++i;
        }
        if (tag < 0 || kp < 0 || a < 0) return false;
        bool first = true;
        for (JsonVariantConst r : rows) {
            if (first) {
                first = false;
                continue;
            }
            readings.push_back({r[tag] | "", {solar_detail::numberOf(r[kp]), int(solar_detail::numberOf(r[a]))}});
        }
    } else {
        for (JsonVariantConst r : rows)
            readings.push_back({r["time_tag"] | "", {solar_detail::numberOf(r["Kp"]), int(solar_detail::numberOf(r["a_running"]))}});
    }
    if (readings.empty()) return false;
    std::stable_sort(readings.begin(), readings.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    const size_t from = readings.size() > 8 ? readings.size() - 8 : 0;
    s.kpCount = 0;
    for (size_t i = from; i < readings.size(); ++i) {
        KpReading& k = s.kp[s.kpCount++];
        strncpy(k.timeTag, readings[i].first.c_str(), sizeof k.timeTag - 1);
        k.kp = float(readings[i].second.first);
    }
    s.k = float(readings.back().second.first);
    s.a = readings.back().second.second;
    return true;
}

// xray_class(): the GOES 0.1-0.8 nm flux as a flare class.
inline void xrayClass(double flux, char (&out)[12]) {
    if (flux < 0) {
        strcpy(out, "UNKNOWN");
        return;
    }
    static const struct { char label; double base; } classes[] = {{'X', 1e-4}, {'M', 1e-5}, {'C', 1e-6}, {'B', 1e-7}, {'A', 1e-8}};
    for (const auto& c : classes)
        if (flux >= c.base || c.label == 'A') {
            snprintf(out, sizeof out, "%c%.1f", c.label, flux / c.base);
            return;
        }
}

// xrays-6-hour.json: the newest 0.1-0.8 nm reading. A byte-range response
// can start mid-row, and SWPC has served the file without its closing ']'.
inline bool parseXray(const char* json, char (&out)[12]) {
    if (!json) return false;
    while (std::isspace(static_cast<unsigned char>(*json))) ++json;
    std::string array;
    if (*json != '[') {
        json = strstr(json, "{\"time_tag\"");
        if (!json) return false;
        array = '[';
        array += json;
        json = array.c_str();
    }
    JsonDocument filter;
    filter[0]["time_tag"] = true;
    filter[0]["flux"] = true;
    filter[0]["energy"] = true;
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, json, DeserializationOption::Filter(filter));
    if ((error && error != DeserializationError::IncompleteInput) || !doc.is<JsonArrayConst>()) return false;
    std::string bestTag;
    double bestFlux = -1;
    bool found = false;
    for (JsonVariantConst r : doc.as<JsonArrayConst>()) {
        const char* energy = r["energy"] | "";
        const char* tag = r["time_tag"] | "";
        if (strcmp(energy, "0.1-0.8nm") || !r["flux"].is<double>() || !*tag) continue;  // a cut-off row has no tag yet
        const double flux = r["flux"].as<double>();
        if (flux < 0) continue;
        if (!found || std::string(tag) > bestTag) {  // max(): the first of equal tags
            bestTag = tag;
            bestFlux = flux;
            found = true;
        }
    }
    if (!found) return false;
    xrayClass(bestFlux, out);
    return true;
}

// 3-day-forecast.txt: per-day Kp maxima from the "00-03UT ..." rows.
inline void parseForecastKp(const char* text, float (&maxima)[3]) {
    using namespace solar_detail;
    maxima[0] = maxima[1] = maxima[2] = 0;
    for (const std::string& line : lines(text ? text : "")) {
        if (!(digitsAt(line, 0, 2) && line.size() >= 7 && line[2] == '-' && digitsAt(line, 3, 2) && line[5] == 'U'
              && line[6] == 'T'))
            continue;
        int column = 0;
        for (size_t i = 0; i < line.size() && column < 3;) {  // \d+\.\d+
            if (!isdigit(static_cast<unsigned char>(line[i]))) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < line.size() && isdigit(static_cast<unsigned char>(line[j]))) ++j;
            if (j + 1 < line.size() && line[j] == '.' && isdigit(static_cast<unsigned char>(line[j + 1]))) {
                size_t k = j + 1;
                while (k < line.size() && isdigit(static_cast<unsigned char>(line[k]))) ++k;
                const float v = float(atof(line.substr(i, k - i).c_str()));
                maxima[column] = std::max(maxima[column], v);
                ++column;
                i = k;
            } else {
                i = j;
            }
        }
    }
}

// noaa-scales.json: "0" (now) to "3".
inline bool parseScales(const char* json, LocalSolar& s) {
    JsonDocument doc;
    if (!json || deserializeJson(doc, json) || !doc.is<JsonObjectConst>()) return false;
    for (int i = 0; i < 4; ++i) {
        JsonObjectConst day = doc[std::to_string(i)];
        ScaleDay& d = s.scales[i];
        d = ScaleDay{};
        strncpy(d.dateStamp, day["DateStamp"] | "UNKNOWN", sizeof d.dateStamp - 1);
        d.hasR = !day["R"]["MinorProb"].isNull();
        d.rMinorProb = solar_detail::stringOf(day["R"]["MinorProb"]);
        d.hasS = !day["S"]["Prob"].isNull();
        d.sProb = solar_detail::stringOf(day["S"]["Prob"]);
        d.gScale = solar_detail::stringOf(day["G"]["Scale"]);
    }
    JsonObjectConst now = doc["0"];
    s.scaleNow[0] = solar_detail::stringOf(now["R"]["Scale"]);
    s.scaleNow[1] = solar_detail::stringOf(now["S"]["Scale"]);
    s.scaleNow[2] = solar_detail::stringOf(now["G"]["Scale"]);
    return true;
}

// ---- alerts -------------------------------------------------------------------

namespace solar_detail {

// (?m)^(?:(?:EXTENDED|CONTINUED) )?(?:WATCH|WARNING|ALERT):[^\r\n]+
inline bool alertTitle(const std::string& message, std::string& title) {
    for (const std::string& line : lines(message.c_str())) {
        std::string rest = line;
        if (startsWith(rest, "EXTENDED ")) rest = rest.substr(9);
        else if (startsWith(rest, "CONTINUED ")) rest = rest.substr(10);
        for (const char* kind : {"WATCH:", "WARNING:", "ALERT:"})
            if (startsWith(rest, kind) && rest.size() > strlen(kind)) {
                title = line;
                return true;
            }
    }
    return false;
}

inline std::string strip(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// (?:Valid To|Now Valid Until):\s*(\d{4} \w{3} \d{1,2} \d{4}) UTC
inline bool alertEnd(const std::string& m, int64_t& epoch) {
    for (const char* key : {"Valid To:", "Now Valid Until:"}) {
        for (size_t at = m.find(key); at != std::string::npos; at = m.find(key, at + 1)) {
            size_t i = at + strlen(key);
            while (i < m.size() && isspace(static_cast<unsigned char>(m[i]))) ++i;
            if (!digitsAt(m, i, 4) || i + 4 >= m.size() || m[i + 4] != ' ') continue;
            const int year = atoi(m.substr(i, 4).c_str());
            size_t j = i + 5, k = j;
            while (k < m.size() && isWord(m[k])) ++k;
            if (k - j != 3 || k >= m.size() || m[k] != ' ') continue;
            const int month = monthIndex(m.substr(j, 3));
            size_t d0 = k + 1, d1 = d0;
            while (d1 < m.size() && d1 - d0 < 2 && isdigit(static_cast<unsigned char>(m[d1]))) ++d1;
            if (d1 == d0 || d1 >= m.size() || m[d1] != ' ' || !digitsAt(m, d1 + 1, 4)) continue;
            if (m.compare(d1 + 5, 4, " UTC") != 0) continue;
            const int day = atoi(m.substr(d0, d1 - d0).c_str());
            const int hhmm = atoi(m.substr(d1 + 1, 4).c_str());
            if (!month || hhmm / 100 > 23 || hhmm % 100 > 59) continue;
            epoch = (daysFromCivil(year, unsigned(month), unsigned(day)) * 1440 + (hhmm / 100) * 60 + hhmm % 100) * 60;
            return true;
        }
    }
    return false;
}

// ([A-Z][a-z]{2}) (\d{1,2}):\s+(G[1-5]) -> (month, day text, level)
struct WatchDay {
    std::string month, day, level;
};

inline std::vector<WatchDay> watchDays(const std::string& m) {
    std::vector<WatchDay> out;
    for (size_t i = 0; i + 3 < m.size(); ++i) {
        if (!isupper(static_cast<unsigned char>(m[i])) || !islower(static_cast<unsigned char>(m[i + 1]))
            || !islower(static_cast<unsigned char>(m[i + 2])) || m[i + 3] != ' ')
            continue;
        size_t d0 = i + 4, d1 = d0;
        while (d1 < m.size() && d1 - d0 < 2 && isdigit(static_cast<unsigned char>(m[d1]))) ++d1;
        if (d1 == d0 || d1 >= m.size() || m[d1] != ':') continue;
        size_t s = d1 + 1;
        if (s >= m.size() || !isspace(static_cast<unsigned char>(m[s]))) continue;
        while (s < m.size() && isspace(static_cast<unsigned char>(m[s]))) ++s;
        if (s + 1 < m.size() && m[s] == 'G' && m[s + 1] >= '1' && m[s + 1] <= '5') {
            out.push_back({m.substr(i, 3), m.substr(d0, d1 - d0), m.substr(s, 2)});
            i = s + 1;
        }
    }
    return out;
}

}  // namespace solar_detail

struct AlertRow {
    std::string productId, issue, message;
};

// active_alerts(): the latest product of each kind only; warnings until they
// expire, watches with forecast days from today, alerts for 6 h (continued
// alerts 36 h). Sorted with electron notices last.
inline std::vector<std::string> activeAlerts(const std::vector<AlertRow>& rows, int64_t now) {
    using namespace solar_detail;
    std::vector<const AlertRow*> sorted;
    for (const AlertRow& r : rows) sorted.push_back(&r);
    std::stable_sort(sorted.begin(), sorted.end(), [](const AlertRow* a, const AlertRow* b) { return a->issue < b->issue; });
    std::vector<std::pair<std::string, const AlertRow*>> latest;  // in first-seen order, like a dict
    for (const AlertRow* r : sorted) {
        auto it = std::find_if(latest.begin(), latest.end(), [&](const auto& p) { return p.first == r->productId; });
        if (it == latest.end()) latest.push_back({r->productId, r});
        else it->second = r;
    }
    const LocalDate today = civilFromDays(now / 86400);
    const int64_t todayDays = daysFromCivil(today.year, today.month, today.day);
    std::vector<std::string> result;
    for (const auto& [id, row] : latest) {
        const std::string& message = row->message;
        if (upper(message).find("CANCEL") != std::string::npos) continue;
        std::string title;
        if (!alertTitle(message, title)) continue;
        title = strip(title);
        int64_t expires = 0, issue = 0;
        if (alertEnd(message, expires)) {
            if (expires > now) result.push_back(title + " \xc2\xb7 UNTIL " + utcStamp(expires));
        } else if (startsWith(title, "WATCH:")) {
            if (!issueEpoch(row->issue.c_str(), issue)) continue;
            const LocalDate issued = civilFromDays(issue / 86400);
            std::vector<std::string> days;
            for (const WatchDay& w : watchDays(message)) {
                const int month = monthIndex(w.month), day = atoi(w.day.c_str());
                if (!month) continue;
                int64_t best = 0;
                bool first = true;
                for (int year = issued.year - 1; year <= issued.year + 1; ++year) {  // the year nearest the issue
                    const int64_t at = daysFromCivil(year, unsigned(month), unsigned(day)) * 86400;
                    if (first || std::llabs(at - issue) < std::llabs(best - issue)) best = at;
                    first = false;
                }
                if (best / 86400 >= todayDays) days.push_back(w.month + " " + w.day + " " + w.level);
            }
            if (!days.empty()) {
                std::string joined;
                for (const std::string& d : days) joined += (joined.empty() ? "" : ", ") + d;
                result.push_back(title + " \xc2\xb7 " + joined);
            }
        } else if (title.find("ALERT:") != std::string::npos) {
            if (!issueEpoch(row->issue.c_str(), issue)) continue;
            const int64_t age = now - issue, limit = (startsWith(title, "CONTINUED") ? 36 : 6) * 3600;
            if (age >= 0 && age < limit) result.push_back(title + " \xc2\xb7 " + utcStamp(issue));
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    std::stable_sort(result.begin(), result.end(), [](const std::string& a, const std::string& b) {
        return (lower(a).find("electron") != std::string::npos) < (lower(b).find("electron") != std::string::npos);
    });
    return result;
}

inline bool parseAlertRows(const char* json, std::vector<AlertRow>& rows) {
    JsonDocument doc;
    if (!json || deserializeJson(doc, json) || !doc.is<JsonArrayConst>()) return false;
    rows.clear();
    for (JsonVariantConst r : doc.as<JsonArrayConst>())
        rows.push_back({r["product_id"] | "", r["issue_datetime"] | "", r["message"] | ""});
    return true;
}

namespace solar_detail {

// findall [GRS]([1-5]) on the alert (any position).
inline int maxScale(const std::string& a) {
    int best = 0;
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if ((a[i] == 'G' || a[i] == 'R' || a[i] == 'S') && a[i + 1] >= '1' && a[i + 1] <= '5') best = std::max(best, a[i + 1] - '0');
    return best;
}

// findall \b[GRS][1-5]\b, unique in order.
inline std::vector<std::string> scaleWords(const std::string& u) {
    std::vector<std::string> out;
    for (size_t i = 0; i + 1 < u.size(); ++i) {
        if (!((u[i] == 'G' || u[i] == 'R' || u[i] == 'S') && u[i + 1] >= '1' && u[i + 1] <= '5')) continue;
        if ((i > 0 && isWord(u[i - 1])) || (i + 2 < u.size() && isWord(u[i + 2]))) continue;
        const std::string w = u.substr(i, 2);
        if (std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
    }
    return out;
}

// findall \b(?:JAN|...|DEC) \d{1,2}(?: \d{4} UTC)?, unique in order.
inline std::vector<std::string> dateWords(const std::string& u) {
    static const char* const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    std::vector<std::string> out;
    for (size_t i = 0; i + 4 < u.size(); ++i) {
        if (i > 0 && isWord(u[i - 1])) continue;
        bool month = false;
        for (const char* m : months) month = month || u.compare(i, 3, m) == 0;
        if (!month || u[i + 3] != ' ') continue;
        size_t d0 = i + 4, d1 = d0;
        while (d1 < u.size() && d1 - d0 < 2 && isdigit(static_cast<unsigned char>(u[d1]))) ++d1;
        if (d1 == d0) continue;
        size_t end = d1;
        if (d1 + 9 <= u.size() && u[d1] == ' ' && digitsAt(u, d1 + 1, 4) && u.compare(d1 + 5, 4, " UTC") == 0) end = d1 + 9;
        const std::string w = u.substr(i, end - i);
        if (std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
        i = end - 1;
    }
    return out;
}

}  // namespace solar_detail

// alert_summary(): at most two products; label WATCH when both are watches.
inline void alertSummary(const std::vector<std::string>& alerts, std::string& label, std::string& message) {
    using namespace solar_detail;
    if (alerts.empty()) {
        label.clear();
        message = "NO ACTIVE SWPC WATCHES / WARNINGS";
        return;
    }
    std::vector<std::string> chosen = alerts;
    std::stable_sort(chosen.begin(), chosen.end(), [](const std::string& a, const std::string& b) {
        const bool ea = lower(a).find("electron") != std::string::npos, eb = lower(b).find("electron") != std::string::npos;
        if (ea != eb) return ea < eb;
        return -maxScale(a) < -maxScale(b);
    });
    if (chosen.size() > 2) chosen.resize(2);
    bool allWatch = true;
    for (const std::string& a : chosen) allWatch = allWatch && upper(a).find("WATCH:") != std::string::npos;
    label = allWatch ? "WATCH" : "ALERT";
    message.clear();
    for (const std::string& alert : chosen) {
        const std::string u = upper(alert);
        const std::string kind = u.find("WATCH:") != std::string::npos     ? "WATCH"
                               : u.find("WARNING:") != std::string::npos ? "WARNING"
                                                                          : "ALERT";
        auto has = [&](const char* s) { return u.find(s) != std::string::npos; };
        const std::string product = has("GEOMAGNETIC") || has("K-INDEX") ? "GEOMAGNETIC STORM"
                                  : has("X-RAY") || has("RADIO")         ? "RADIO BLACKOUT"
                                  : has("PROTON") || has("RADIATION")    ? "SOLAR RADIATION"
                                  : has("ELECTRON")                      ? "ELECTRON FLUX"
                                                                         : "SPACE WEATHER";
        std::vector<std::string> scales = scaleWords(u);
        if (scales.empty() && has("K-INDEX")) {  // K-INDEX\s+([5-9])
            const size_t at = u.find("K-INDEX");
            size_t i = at + 7;
            if (i < u.size() && isspace(static_cast<unsigned char>(u[i]))) {
                while (i < u.size() && isspace(static_cast<unsigned char>(u[i]))) ++i;
                if (i < u.size() && u[i] >= '5' && u[i] <= '9') scales.push_back("G" + std::to_string(u[i] - '0' - 4));
            }
        }
        std::string summary = kind != label ? kind + " " + product : product;
        for (const std::string& s : scales) summary += " " + s;
        const std::vector<std::string> dates = dateWords(u);
        if (!dates.empty()) {
            summary += " \xc2\xb7 ";
            if (has("UNTIL")) summary += "UNTIL ";
            for (size_t i = 0; i < dates.size(); ++i) summary += (i ? ", " : "") + dates[i];
        }
        message += (message.empty() ? "" : " / ") + summary;
    }
}

// ---- estimates and labels -------------------------------------------------------

struct BandEstimate {
    const char* name;
    const char* day;
    const char* night;
};

struct HfEstimate {
    BandEstimate bands[4];
    int muf;
    const char* noise;
    const char* field;
};

// conditions(): HF estimates, not a propagation model.
inline HfEstimate conditions(double sfi, double k, int a, const char* xray) {
    static const char* const ratings[] = {"Poor", "Fair", "Good"};
    static const struct { const char* name; int fair, good, night; } bands[] = {
        {"80\xe2\x80\x93" "40 M", 0, 999, 2}, {"30\xe2\x80\x93" "20 M", 70, 100, 2},
        {"17\xe2\x80\x93" "15 M", 90, 120, 1}, {"12\xe2\x80\x93" "10 M", 120, 170, 0}};
    const int penalty = k >= 6 || a >= 50 ? 2 : k >= 4 || a >= 20 ? 1 : 0;
    const int absorption = xray[0] == 'X' ? 2 : xray[0] == 'M' ? 1 : 0;
    HfEstimate e{};
    for (int i = 0; i < 4; ++i) {
        const int day = sfi >= bands[i].good ? 2 : sfi >= bands[i].fair ? 1 : 0;
        e.bands[i] = {bands[i].name, ratings[std::max(0, day - penalty - absorption)],
                      ratings[std::max(0, bands[i].night - penalty)]};
    }
    e.muf = roundHalfEven(std::max(10.0, std::min(45.0, 10 + 0.1 * sfi)));
    e.noise = k < 3 ? "S1\xe2\x80\x93S2" : k < 5 ? "S3\xe2\x80\x93S4" : k < 7 ? "S5\xe2\x80\x93S7" : "S8\xe2\x80\x93S9";
    e.field = k < 3 ? "QUIET" : k < 5 ? "UNSETTLED" : "STORM";
    return e;
}

inline std::string gridSquare(double lat, double lon) {
    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) return "";
    const double x = std::min(lon + 180, 359.999999), y = std::min(lat + 90, 179.999999);
    std::string g;
    g += char('A' + int(std::floor(x / 20)));
    g += char('A' + int(std::floor(y / 10)));
    g += char('0' + int(std::floor(std::fmod(x, 20) / 2)));
    g += char('0' + int(std::floor(std::fmod(y, 10))));
    return g;
}

inline int integerK(double k) { return std::max(0, std::min(9, int(k + 0.5))); }

inline std::string kpNotation(double k) {
    const int thirds = std::max(0, std::min(27, int(k * 3 + 0.5)));
    const int whole = thirds / 3, remainder = thirds % 3;
    return remainder == 0 ? std::to_string(whole) : remainder == 1 ? std::to_string(whole) + "+" : std::to_string(whole + 1) + "-";
}

enum class KColour { Green, Gold, Red };
inline KColour kColour(double k) { return k <= 3 ? KColour::Green : k < 5 ? KColour::Gold : KColour::Red; }

inline std::string forecastLabel(double k, const std::string& g) {
    const int level = atoi(g.c_str());
    if (level) return "G" + std::to_string(level) + " STORM";
    if (k >= 5) return "G" + std::to_string(std::min(5, std::max(1, integerK(k) - 4))) + " STORM";
    return k > 4 ? "ACTIVE" : k > 3 ? "UNSETTLED" : "QUIET";
}

}  // namespace lcd::local
