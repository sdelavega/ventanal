// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include <cstdint>
#include <cstring>

constexpr int ESP_OK = 0;
struct esp_http_client_config_t {
    const char* url;
    const char* user_agent;
    int timeout_ms;
    bool disable_auto_redirect;
    int (*crt_bundle_attach)(void*);
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
};
inline FakeHttpsResponse fakeHttps;
using esp_http_client_handle_t = FakeHttpsResponse*;
inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*) { return &fakeHttps; }
inline int esp_http_client_set_header(esp_http_client_handle_t c, const char*, const char* value) {
    c->range = value;
    return ESP_OK;
}
inline int esp_http_client_open(esp_http_client_handle_t c, int) { return c->openResult; }
inline int esp_http_client_set_timeout_ms(esp_http_client_handle_t, int) { return ESP_OK; }
inline int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { return c->contentLength; }
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
