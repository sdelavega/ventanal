// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/local/solar_data.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace lcd::local;

static std::string fixture(const std::string& name) {
    std::ifstream in("fixtures/standalone/" + name);
    assert(in && "run from firmware/test");
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

static bool near(double a, double b) { return std::fabs(a - b) < 1e-4; }

static std::string text(JsonVariantConst v) { return v.isNull() ? "" : solar_detail::stringOf(v); }

// The server's fetch_solar() of the same captures (tools/standalone_expected.py).
static void solar_matches_server() {
    JsonDocument e;
    assert(!deserializeJson(e, fixture("expected-solar.json")));
    LocalSolar s{};
    assert(parseDailyIndices(fixture("swpc-daily.txt").c_str(), s.sfi, s.ssn));
    assert(parseKp(fixture("swpc-kp.json").c_str(), s));
    assert(parseXray(fixture("swpc-xray.json").c_str(), s.xray));
    parseForecastKp(fixture("swpc-forecast.txt").c_str(), s.kpMax);
    assert(parseScales(fixture("swpc-scales.json").c_str(), s));
    std::vector<AlertRow> rows;
    assert(parseAlertRows(fixture("swpc-alerts.json").c_str(), rows));
    s.alerts = activeAlerts(rows, e["now_epoch"].as<int64_t>());

    assert(near(s.sfi, e["sfi"].as<double>()) && s.ssn == e["ssn"].as<int>());
    assert(std::string(s.xray) == e["xray"].as<const char*>());
    assert(near(s.k, e["k"].as<double>()) && s.a == e["a"].as<int>() && integerK(s.k) == e["integer_k"].as<int>());
    JsonArrayConst kp = e["kp"];
    assert(s.kpCount == int(kp.size()));
    for (int i = 0; i < s.kpCount; ++i)
        assert(std::string(s.kp[i].timeTag) == kp[i][0].as<const char*>() && near(s.kp[i].kp, kp[i][1].as<double>()));
    assert(s.alerts.size() == e["alerts"].size());
    assert(s.scaleNow[0] == text(e["scales_now"]["R"]) && s.scaleNow[1] == text(e["scales_now"]["S"])
           && s.scaleNow[2] == text(e["scales_now"]["G"]));
    JsonArrayConst forecast = e["forecast"];
    for (int i = 0; i < 3; ++i) {
        const ScaleDay& d = s.scales[i + 1];
        JsonObjectConst f = forecast[i];
        assert(std::string(d.dateStamp) == f["date"].as<const char*>());
        assert(near(s.kpMax[i], f["kp_max"].as<double>()) && kpNotation(s.kpMax[i]) == f["kp_notation"].as<const char*>());
        assert(forecastLabel(s.kpMax[i], d.gScale) == f["label"].as<const char*>());
        assert(d.rMinorProb == text(f["r"]) && d.sProb == text(f["s"]) && d.gScale == text(f["g"]));
    }
    const HfEstimate est = conditions(s.sfi, s.k, s.a, s.xray);
    JsonObjectConst c = e["conditions"];
    for (int i = 0; i < 4; ++i) {
        assert(std::string(est.bands[i].name) == c["bands"][i][0].as<const char*>());
        assert(std::string(est.bands[i].day) == c["bands"][i][1].as<const char*>());
        assert(std::string(est.bands[i].night) == c["bands"][i][2].as<const char*>());
    }
    assert(est.muf == c["muf"].as<int>() && std::string(est.noise) == c["noise"].as<const char*>());
    assert(std::string(est.field) == c["field"].as<const char*>());
    std::string label, message;
    alertSummary(s.alerts, label, message);
    assert(label == e["alert_label"].as<const char*>() && message == e["alert_message"].as<const char*>());
    assert(gridSquare(34.0901, -118.4065) == e["grid_90210"].as<const char*>());
    puts("solar_matches_server ok");
}

// active_alerts() at a moment with five live products, and alert_summary()
// over hand-picked combinations.
static void alerts_match_server() {
    JsonDocument e;
    assert(!deserializeJson(e, fixture("expected-solar-alerts.json")));
    std::vector<AlertRow> rows;
    assert(parseAlertRows(fixture("swpc-alerts.json").c_str(), rows));
    const std::vector<std::string> active = activeAlerts(rows, e["now_epoch"].as<int64_t>());
    JsonArrayConst want = e["active"];
    assert(active.size() == want.size());
    for (size_t i = 0; i < active.size(); ++i) {
        if (active[i] != want[i].as<const char*>()) printf("active %zu: %s != %s\n", i, active[i].c_str(), want[i].as<const char*>());
        assert(active[i] == want[i].as<const char*>());
    }
    for (JsonObjectConst c : e["cases"].as<JsonArrayConst>()) {
        std::vector<std::string> alerts;
        for (JsonVariantConst a : c["alerts"].as<JsonArrayConst>()) alerts.push_back(a.as<const char*>());
        std::string label, message;
        alertSummary(alerts, label, message);
        if (message != c["message"].as<const char*>()) printf("summary: %s != %s\n", message.c_str(), c["message"].as<const char*>());
        assert(label == c["label"].as<const char*>() && message == c["message"].as<const char*>());
    }
    puts("alerts_match_server ok");
}

// SWPC served xrays-6-hour.json without its closing ']' (2026-11-19).
static void xray_tolerates_a_cut_off_file() {
    std::string whole = fixture("swpc-xray.json");
    char full[12], cut[12], midRow[12];
    assert(parseXray(whole.c_str(), full));
    while (!whole.empty() && (whole.back() == '\n' || whole.back() == ' ')) whole.pop_back();
    assert(whole.back() == ']');
    assert(parseXray(whole.substr(0, whole.size() - 1).c_str(), cut) && !strcmp(full, cut));
    const size_t lastRow = whole.rfind("{\"time_tag\"");
    assert(lastRow != std::string::npos);
    assert(parseXray(whole.substr(0, lastRow + 20).c_str(), midRow));  // a row cut in its tag
    assert(!parseXray("", cut) && !parseXray("{}", cut));
    puts("xray_tolerates_a_cut_off_file ok");
}

static void xray_reads_a_tail() {
    const std::string whole = fixture("swpc-xray.json");
    char full[12], tail[12];
    assert(parseXray(whole.c_str(), full) && !strcmp(full, "B4.9"));
    for (size_t size : {8192u, 3000u}) {
        assert(whole.size() > size);
        assert(parseXray(whole.c_str() + whole.size() - size, tail) && !strcmp(full, tail));
    }

    const size_t firstRow = whole.find("{\"time_tag\"", whole.size() - 3000);
    const size_t lastRow = whole.rfind("{\"time_tag\"");
    assert(firstRow != std::string::npos && firstRow < lastRow);
    const std::string cut = " \t\n" + whole.substr(firstRow + 20, lastRow - firstRow);
    assert(parseXray(cut.c_str(), tail));
    assert(!parseXray("", tail));
    assert(!parseXray(" \"flux\": 4.9e-7}]", tail));
    puts("xray_reads_a_tail ok");
}

static void labels() {
    assert(kpNotation(2.67) == "3-" && kpNotation(3.33) == "3+" && kpNotation(3.0) == "3" && kpNotation(9.5) == "9");
    assert(forecastLabel(5.67, "") == "G2 STORM" && forecastLabel(2, "3") == "G3 STORM" && forecastLabel(4.33, "") == "ACTIVE");
    char c[12];
    xrayClass(2.3e-5, c);
    assert(!strcmp(c, "M2.3"));
    xrayClass(4e-9, c);
    assert(!strcmp(c, "A0.4"));
    assert(gridSquare(91, 0).empty());
    puts("labels ok");
}

int main() {
    solar_matches_server();
    alerts_match_server();
    xray_tolerates_a_cut_off_file();
    xray_reads_a_tail();
    labels();
}
