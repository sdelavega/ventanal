// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../common/setup_portal/locate.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace portal;

static std::string fixture(const char* name) {
    std::ifstream in(std::string("fixtures/standalone/") + name);
    assert(in && "run from firmware/test");
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

static void zippopotam_parses_place() {
    float lat = 0, lon = 0;
    char place[48];
    assert(parseZippopotam(fixture("zippopotam-90210.json").c_str(), lat, lon, place));
    assert(std::fabs(lat - 34.0901f) < 1e-4f && std::fabs(lon + 118.4065f) < 1e-4f);
    assert(!strcmp(place, "Beverly Hills, CA"));
    assert(!parseZippopotam("{}", lat, lon, place));
    assert(!parseZippopotam("{\"places\":[{\"latitude\":\"x\",\"longitude\":\"1\"}]}", lat, lon, place));
    assert(!parseZippopotam("<html>", lat, lon, place));
    // A long name is cut on a character boundary, never mid UTF-8 sequence.
    const std::string longName = "{\"places\":[{\"place name\":\"" + std::string(46, 'a')
        + "\xc3\xa9xx\",\"state abbreviation\":\"CA\",\"latitude\":\"1\",\"longitude\":\"2\"}]}";
    assert(parseZippopotam(longName.c_str(), lat, lon, place));
    assert(strlen(place) == 46 && place[45] == 'a');  // the 47th byte would split the é
}

static void zip_urls_use_five_digits() {
    char url[128];
    zipUrl("90210-1234", url, sizeof url);
    assert(!strcmp(url, "https://api.zippopotam.us/us/90210"));
    timezoneUrl(34.0901f, -118.4065f, url, sizeof url);
    assert(!strcmp(url, "https://api.open-meteo.com/v1/forecast?latitude=34.0901&longitude=-118.4065"
                        "&timezone=auto&forecast_days=1"));
}

static void open_meteo_names_the_zone() {
    char iana[64];
    assert(parseOpenMeteoTimezone(fixture("open-meteo-tz-90210.json").c_str(), iana));
    assert(!strcmp(iana, "America/Los_Angeles"));
    assert(!parseOpenMeteoTimezone("{\"latitude\":1}", iana));
}

static void resolves_the_sample_zip() {
    LocationResult r = locationFromZip(200, fixture("zippopotam-90210.json").c_str());
    assert(r.why == LocationResult::Ok && !r.ok);  // not done until the time zone is known
    finishLocation(r, 200, fixture("open-meteo-tz-90210.json").c_str());
    assert(r.ok && r.why == LocationResult::Ok);
    assert(!strcmp(r.place, "Beverly Hills, CA") && !strcmp(r.posixTz, "PST8PDT,M3.2.0,M11.1.0"));
}

static void unknown_zip_is_reported() {
    const LocationResult r = locationFromZip(404, fixture("zippopotam-00000.json").c_str());
    assert(!r.ok && r.why == LocationResult::ZipUnknown);
    assert(locationFromZip(200, "{}").why == LocationResult::ZipUnknown);
}

static void network_failures_are_not_blamed_on_the_zip() {
    assert(locationFromZip(-1, "").why == LocationResult::Network);
    assert(locationFromZip(503, "busy").why == LocationResult::Network);
    LocationResult r = locationFromZip(200, fixture("zippopotam-90210.json").c_str());
    finishLocation(r, -1, "");
    assert(!r.ok && r.why == LocationResult::Network);
    r = locationFromZip(200, fixture("zippopotam-90210.json").c_str());
    finishLocation(r, 200, "{\"timezone\":\"Europe/Paris\"}");
    assert(!r.ok && r.why == LocationResult::TzUnknown);
}

static void posix_table_lookups() {
    assert(!strcmp(posixForIana("America/New_York"), "EST5EDT,M3.2.0,M11.1.0"));
    assert(!strcmp(posixForIana("America/Phoenix"), "MST7"));
    assert(!strcmp(posixForIana("Pacific/Honolulu"), "HST10"));
    assert(!strcmp(posixForIana("Etc/UTC"), "UTC0"));
    assert(!posixForIana("Europe/Paris") && !posixForIana("") && !posixForIana("America/New_Yor"));
    for (size_t i = 1; i < sizeof tzPosixTable / sizeof tzPosixTable[0]; ++i)
        assert(strcmp(tzPosixTable[i - 1].iana, tzPosixTable[i].iana) < 0);
}

int main() {
    zippopotam_parses_place();
    zip_urls_use_five_digits();
    open_meteo_names_the_zone();
    resolves_the_sample_zip();
    unknown_zip_is_reported();
    network_failures_are_not_blamed_on_the_zip();
    posix_table_lookups();
    puts("locate: ZIP geocoding, Open-Meteo time zone, POSIX table and failure reasons passed");
}
