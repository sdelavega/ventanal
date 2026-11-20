// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "https_get.h"

#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

HttpsResult httpsGet(const char* url, char* buffer, size_t cap, uint32_t deadlineMs, void (*feed)(),
                     const char* range) {
    HttpsResult result;
    if (!cap || strncmp(url, "https://", 8)) return result;
    buffer[0] = '\0';
    esp_http_client_config_t config{};
    config.url = url;
    config.user_agent = publicUserAgent;
    constexpr uint32_t SocketTimeoutMs = 5000;
    if (!deadlineMs) return result;
    config.timeout_ms = std::min(SocketTimeoutMs, deadlineMs);
    config.disable_auto_redirect = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return result;
    const bool rangeReady = !range || esp_http_client_set_header(client, "Range", range) == ESP_OK;
    const uint32_t started = millis();
    int64_t announced = -1;
    if (feed) feed();
    if (rangeReady && esp_http_client_open(client, 0) == ESP_OK) {
        if (feed) feed();
        const uint32_t elapsed = millis() - started;
        if (elapsed < deadlineMs) {
            esp_http_client_set_timeout_ms(client, std::min(SocketTimeoutMs, deadlineMs - elapsed));
            announced = esp_http_client_fetch_headers(client);
        }
    }
    if (announced >= 0) {
        result.status = esp_http_client_get_status_code(client);
        const auto complete = [&] {
            // fetch_headers returns zero for chunked/unknown length. A known
            // length must count bytes actually copied, including ranged tails.
            return announced > 0 ? uint64_t(announced) == result.length
                                 : esp_http_client_is_complete_data_received(client);
        };
        if (announced > 0 && uint64_t(announced) >= cap) result.truncated = true;
        const auto read = [&](char* dest, size_t bytes) {
            if (feed) feed();
            const uint32_t elapsed = millis() - started;
            if (elapsed >= deadlineMs) return -1;
            esp_http_client_set_timeout_ms(client, std::min(SocketTimeoutMs, deadlineMs - elapsed));
            return esp_http_client_read(client, dest, bytes);
        };
        while (!result.truncated) {
            if (result.length + 1 >= cap) {
                char probe;
                result.truncated = read(&probe, 1) > 0;
                break;
            }
            // Read in chunks to check the deadline and feed the watchdog.
            constexpr size_t Chunk = 1024;
            const int got = read(buffer + result.length, std::min(Chunk, cap - 1 - result.length));
            if (got <= 0) break;
            result.length += size_t(got);
            if (complete()) break;
        }
        if (!result.truncated && !complete()) result.status = -1;
    }
    buffer[result.length] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}
