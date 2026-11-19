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
    config.timeout_ms = 5000;
    config.disable_auto_redirect = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return result;
    const bool rangeReady = !range || esp_http_client_set_header(client, "Range", range) == ESP_OK;
    const uint32_t started = millis();
    int64_t announced = -1;
    if (rangeReady && esp_http_client_open(client, 0) == ESP_OK
        && (announced = esp_http_client_fetch_headers(client)) >= 0) {
        result.status = esp_http_client_get_status_code(client);
        // A body the server says is too big is refused before reading it.
        if (announced > 0 && size_t(announced) >= cap) result.truncated = true;
        while (!result.truncated && millis() - started < deadlineMs) {
            if (feed) feed();
            if (result.length + 1 >= cap) {
                char probe;
                result.truncated = esp_http_client_read(client, &probe, 1) > 0;
                break;
            }
            // Small reads: one call for a whole body can block past the task
            // watchdog on a slow connection, and the deadline needs checking.
            constexpr size_t Chunk = 16 * 1024;
            const int got = esp_http_client_read(client, buffer + result.length,
                                                 std::min(Chunk, cap - 1 - result.length));
            if (got < 0) {
                result.status = -1;
                break;
            }
            result.length += size_t(got);
            if (esp_http_client_is_complete_data_received(client) || got == 0) break;
        }
        if (millis() - started >= deadlineMs && !esp_http_client_is_complete_data_received(client))
            result.status = -1;  // ran out of time mid-body
    }
    buffer[result.length] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}
