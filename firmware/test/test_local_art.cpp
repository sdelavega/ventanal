// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../lcd43b/local/art_day.h"
#include "../lcd43b/local/art_compose.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace lcd::local;

static std::string fixture(const std::string& name) {
    std::ifstream in("fixtures/standalone/" + name);
    assert(in && "run from firmware/test");
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// Every candidate the board finds, keyed by source:id.
static std::map<std::string, ArtCandidate> boardCandidates() {
    std::map<std::string, ArtCandidate> found;
    std::vector<ArtCandidate> list(16);
    int n = parseAicSearch(fixture("aic-search-winter.json").c_str(), list.data(), 16);
    for (int i = 0; i < n; ++i) found[std::string("aic:") + list[i].objectId] = list[i];
    n = parseClevelandSearch(fixture("cleveland-search-winter.json").c_str(), list.data(), 16);
    for (int i = 0; i < n; ++i) found[std::string("cleveland:") + list[i].objectId] = list[i];
    uint32_t ids[8];
    n = parseMetSearchIds(fixture("met-search-winter.json").c_str(), ids, 8);
    assert(n == 5 && ids[0] == 195823 && ids[4] == 54428);
    for (int i = 0; i < n; ++i) {
        ArtCandidate c;
        if (parseMetObject(fixture("met-object-" + std::to_string(ids[i]) + ".json").c_str(), c))
            found[std::string("met:") + c.objectId] = c;
    }
    return found;
}

// The server's AICSource / ClevelandSource / MetSource over the same fixtures.
static void candidates_match_the_server() {
    JsonDocument expected;
    assert(!deserializeJson(expected, fixture("expected-art-winter.json")));
    const auto found = boardCandidates();
    JsonArrayConst want = expected["candidates"];
    assert(found.size() == want.size());
    for (JsonObjectConst w : want) {
        const std::string key = std::string(w["source"].as<const char*>()) + ":" + w["object_id"].as<const char*>();
        const auto it = found.find(key);
        assert(it != found.end());
        const ArtCandidate& c = it->second;
        assert(!strcmp(c.title, w["title"].as<const char*>()) || strlen(c.title) == sizeof c.title - 1);
        assert(!strcmp(c.artist, w["artist"].as<const char*>()));
        assert(!strcmp(c.date, w["date"].as<const char*>()));
        assert(!strcmp(c.museum, w["museum"].as<const char*>()));
    }
}

// Images sized for an 800x480 screen, not the host's print-size files.
static void image_urls_are_screen_sized() {
    const auto found = boardCandidates();
    const ArtCandidate& aic = found.at("aic:10932");  // thumbnail 1686 x 1124 at full width
    assert(std::string(aic.imageUrl).find("/full/800,/0/default.jpg") != std::string::npos);
    const ArtCandidate& tall = found.at("aic:109780");
    assert(std::string(tall.imageUrl).find("/full/800,/0/default.jpg") != std::string::npos);
    const ArtCandidate& cleveland = found.at("cleveland:153737");
    assert(std::string(cleveland.imageUrl).find("_web.jpg") != std::string::npos);
    const ArtCandidate& met = found.at("met:54428");
    assert(std::string(met.imageUrl).find("/web-large/") != std::string::npos);
    // A very wide piece needs more than 800 px of width to cover 480 px of height.
    assert(aicCoverWidth(4000, 1000) == 1920 && aicCoverWidth(1000, 4000) == 800);
    assert(aicCoverWidth(600, 400) == 600);  // never wider than the original (IIIF refuses to upscale)
}

static void search_urls_encode_terms() {
    char url[256];
    aicSearchUrl("Adoration of the Magi", 12, url, sizeof url);
    assert(strstr(url, "q=Adoration%20of%20the%20Magi&") && strstr(url, "is_public_domain%5D=true"));
    clevelandSearchUrl("winter", 12, url, sizeof url);
    assert(strstr(url, "q=winter&cc0=1&has_image=1&limit=12"));
    metSearchUrl("ice skating", 5, url, sizeof url);
    assert(strstr(url, "/v1.1/search?q=ice%20skating&hasImages=true&offset=0&limit=5"));
    metObjectUrl(54428, url, sizeof url);
    assert(!strcmp(url, "https://collectionapi.metmuseum.org/public/collection/v1/objects/54428"));
}

static void rejects_non_public_and_bad_bodies() {
    ArtCandidate list[4];
    assert(parseAicSearch("<html>", list, 4) == -1);
    assert(parseAicSearch("{\"data\":[{\"id\":1,\"image_id\":\"x\",\"is_public_domain\":false,"
                          "\"thumbnail\":{\"width\":800,\"height\":600}}]}", list, 4) == 0);
    assert(parseClevelandSearch("{\"data\":[{\"id\":2,\"share_license_status\":\"Copyrighted\","
                                "\"images\":{\"web\":{\"url\":\"https://x/y_web.jpg\"}}}]}", list, 4) == 0);
    ArtCandidate c;
    assert(!parseMetObject(fixture("met-object-195823.json").c_str(), c));  // glass: not a painting/drawing/print
    assert(!parseMetObject(fixture("met-object-488285.json").c_str(), c));  // not public domain
    assert(!parseMetObject("{}", c));
}

static void credits_are_cleaned() {
    ArtCandidate list[1];
    const char* body = "{\"data\":[{\"id\":7,\"image_id\":\"x\",\"is_public_domain\":true,"
                       "\"title\":\"Snow\\u0007 at\\nNight\",\"artist_title\":\"A\\u0085B\","
                       "\"thumbnail\":{\"width\":800,\"height\":600}}]}";
    assert(parseAicSearch(body, list, 1) == 1);
    assert(!strcmp(list[0].title, "Snow atNight") && !strcmp(list[0].artist, "AB"));
}

static void seasons_from_the_example() {
    auto at = [](int m, int d) { LocalDate x; x.year = 2026; x.month = uint8_t(m); x.day = uint8_t(d); return x; };
    assert(!strcmp(seasonFor(at(11, 19)).name, "holiday-winter") && !strcmp(seasonFor(at(12, 31)).name, "holiday-winter"));
    assert(!strcmp(seasonFor(at(1, 1)).name, "winter") && !strcmp(seasonFor(at(2, 29)).name, "winter"));
    assert(!strcmp(seasonFor(at(3, 1)).name, "spring") && !strcmp(seasonFor(at(8, 31)).name, "summer"));
    assert(!strcmp(seasonFor(at(10, 31)).name, "autumn"));
}

static void day_pick_is_deterministic() {
    auto at = [](int d) { LocalDate x; x.year = 2026; x.month = 11; x.day = uint8_t(d); return x; };
    assert(daySeed(at(19), "7990") == daySeed(at(19), "7990"));
    assert(daySeed(at(19), "7990") != daySeed(at(20), "7990") && daySeed(at(19), "7990") != daySeed(at(19), "3c0f"));
    assert(!strcmp(dayTerm(at(19), "7990"), dayTerm(at(19), "7990")));
    std::vector<ArtCandidate> pool;
    for (auto& [key, c] : boardCandidates()) pool.push_back(c);
    SeenRing seen;
    int a[4], b[4];
    assert(pickCandidates(pool.data(), int(pool.size()), seen, daySeed(at(19), "7990"), 4, a) == 4);
    assert(pickCandidates(pool.data(), int(pool.size()), seen, daySeed(at(19), "7990"), 4, b) == 4);
    assert(!memcmp(a, b, sizeof a));
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j) assert(a[i] != a[j]);
}

static void no_repeat_within_seen_ring() {
    std::vector<ArtCandidate> pool;
    for (auto& [key, c] : boardCandidates()) pool.push_back(c);
    SeenRing seen;
    LocalDate day;
    day.year = 2026;
    day.month = 11;
    std::vector<uint32_t> shown;
    for (int d = 1; d <= 6; ++d) {  // 24 picks from 25 candidates: no piece twice
        day.day = uint8_t(d);
        int picks[4];
        assert(pickCandidates(pool.data(), int(pool.size()), seen, daySeed(day, "7990"), 4, picks) == 4);
        for (int i : picks) {
            const uint32_t key = candidateKey(pool[size_t(i)]);
            for (uint32_t s : shown) assert(s != key);
            shown.push_back(key);
            seen.add(key);
        }
    }
    int picks[4];  // the pool is used up: seen pieces fill in rather than nothing
    assert(pickCandidates(pool.data(), int(pool.size()), seen, 1234, 4, picks) == 4);
    SeenRing full;
    for (uint32_t k = 0; k < 100; ++k) full.add(k);
    assert(full.count == 64 && !full.contains(0) && full.contains(99));
}

static void art_pass_budget() {
    ArtPassBudget budget;
    assert(budget.keepGoing(0));
    budget.note(-1);   // search fails
    assert(budget.keepGoing(1000));
    budget.note(503);  // an HTTP response resets the transport failure streak
    budget.note(-1);   // the next search fails
    assert(budget.keepGoing(2000));
    budget.note(-1);   // an image download fails too
    assert(!budget.keepGoing(3000));

    ArtPassBudget timed;
    assert(timed.remainingMs(ArtPassBudget::limitMs - 1) == 1);
    assert(!timed.keepGoing(ArtPassBudget::limitMs));
    assert(!timed.keepGoing(ArtPassBudget::limitMs + 1));
    puts("art pass budget: consecutive transport failures and four-minute deadline passed");
}

static void resampler_flushes_short_block_bands() {
    constexpr int height = 32;
    std::vector<uint16_t> pixels(size_t(FRAME_WIDTH) * height);
    ArtResampler resampler;
    assert(resampler.begin({pixels.data(), 0, height}, {0, 0, 2, height}, {0, 0, 5, height}, 5, height, 2));

    for (int y = 0; y < height; y += 2) {
        uint8_t block[2 * 2 * 3] = {};
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 2; ++col) block[(row * 2 + col) * 3] = uint8_t(8 * ((y + row) % 31 + 1));
        resampler.block(0, y, 2, 2, block);
        resampler.block(2, y, 2, 2, block);  // ends one pixel before the assumed width
    }
    resampler.finish();
    for (int y = 0; y < height; ++y) {
        const uint16_t want = rgb565(uint32_t(8 * (y % 31 + 1)) << 16);
        assert(pixels[size_t(y * FRAME_WIDTH)] == want && pixels[size_t(y * FRAME_WIDTH + 1)] == want);
    }
    puts("art resampler: every row rendered with short right-edge blocks");
}

int main() {
    art_pass_budget();
    resampler_flushes_short_block_bands();
    seasons_from_the_example();
    day_pick_is_deterministic();
    no_repeat_within_seen_ring();
    candidates_match_the_server();
    image_urls_are_screen_sized();
    search_urls_encode_terms();
    rejects_non_public_and_bad_bodies();
    credits_are_cleaned();
    puts("local art sources: AIC, Cleveland and Met parsing match the server; screen-sized images");
}
