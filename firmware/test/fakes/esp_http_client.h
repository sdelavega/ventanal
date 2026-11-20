// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

using esp_err_t = int;
constexpr int ESP_OK = 0;
enum { HTTP_EVENT_ON_HEADER };
struct esp_http_client_event_t {
    int event_id;
    void* user_data;
    const char* header_key;
    const char* header_value;
};
struct esp_http_client_config_t {
    const char* url;
    const char* user_agent;
    int timeout_ms;
    bool disable_auto_redirect;
    int (*crt_bundle_attach)(void*);
    void* user_data;
    esp_err_t (*event_handler)(esp_http_client_event_t*);
};
struct FakeHttpsResponse {
    int64_t contentLength = 0;
    int status = 200;
    size_t available = 0;
    size_t read = 0;
    int stallResult = 0;
    bool complete = false;
    const char* range = nullptr;
    int openResult = ESP_OK;
    const char* contentRange = nullptr;
    const char* etag = nullptr;
    const char* modified = nullptr;
};
inline FakeHttpsResponse fakeHttps;
inline esp_http_client_config_t fakeHttpsConfig;
inline std::string fakeRange, fakeIfRange;
using esp_http_client_handle_t = FakeHttpsResponse*;
inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    fakeHttpsConfig = *config;
    fakeRange.clear();
    fakeIfRange.clear();
    return &fakeHttps;
}
inline int esp_http_client_set_header(esp_http_client_handle_t c, const char* name, const char* value) {
    if (!strcmp(name, "Range")) {
        fakeRange = value;
        c->range = fakeRange.c_str();
    }
    if (!strcmp(name, "If-Range")) fakeIfRange = value;
    return ESP_OK;
}
inline int esp_http_client_open(esp_http_client_handle_t c, int) { return c->openResult; }
inline int esp_http_client_set_timeout_ms(esp_http_client_handle_t, int) { return ESP_OK; }
inline int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    const auto emit = [](const char* key, const char* value) {
        if (value && fakeHttpsConfig.event_handler) {
            esp_http_client_event_t event{HTTP_EVENT_ON_HEADER, fakeHttpsConfig.user_data, key, value};
            fakeHttpsConfig.event_handler(&event);
        }
    };
    emit("content-range", c->contentRange);
    emit("etag", c->etag);
    emit("Last-Modified", c->modified);
    return c->contentLength;
}
inline int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }
inline bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {
    return c->complete && c->read == c->available;
}
inline int esp_http_client_read(esp_http_client_handle_t c, char* dst, int size) {
    if (c->available == c->read) return c->stallResult;
    size_t n = c->available - c->read;
    if (n > size_t(size)) n = size_t(size);
    memset(dst, 'a', n);
    c->read += n;
    return int(n);
}
inline void esp_http_client_close(esp_http_client_handle_t) {}
inline void esp_http_client_cleanup(esp_http_client_handle_t) {}
