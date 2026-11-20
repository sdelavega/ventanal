// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "https_get.h"

#include <Arduino.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <strings.h>

namespace {

constexpr uint32_t SocketTimeoutMs = 5000;

class HttpsRequest {
public:
    HttpsRequest(const char* url, uint32_t budget, void (*feed)(),
                 esp_http_client_config_t config = {})
        : budget_(budget), feed_(feed), started_(millis()) {
        if (!budget || strncmp(url, "https://", 8)) return;
        config.url = url;
        config.user_agent = publicUserAgent;
        config.timeout_ms = std::min(SocketTimeoutMs, budget);
        config.disable_auto_redirect = true;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        client = esp_http_client_init(&config);
    }

    ~HttpsRequest() {
        if (client) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }
    }

    bool setHeader(const char* name, const char* value) {
        return !value || !*value || esp_http_client_set_header(client, name, value) == ESP_OK;
    }

    int64_t fetchHeaders() {
        if (!ready() || esp_http_client_open(client, 0) != ESP_OK || !ready()) return -1;
        return esp_http_client_fetch_headers(client);
    }

    int read(char* data, size_t size) {
        return ready() ? esp_http_client_read(client, data, size) : -1;
    }

    esp_http_client_handle_t client = nullptr;

private:
    bool ready() {
        if (!client) return false;
        if (feed_) feed_();
        const uint32_t elapsed = millis() - started_;
        if (elapsed >= budget_) return false;
        esp_http_client_set_timeout_ms(client, std::min(SocketTimeoutMs, budget_ - elapsed));
        return true;
    }

    uint32_t budget_;
    void (*feed_)();
    uint32_t started_;
};

struct RangeHeaders {
    char contentRange[64]{};
    char etag[sizeof(RangeResponse::validator)]{};
    char modified[sizeof(RangeResponse::validator)]{};

    static esp_err_t collect(esp_http_client_event_t* event) {
        if (event->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
        auto& self = *static_cast<RangeHeaders*>(event->user_data);
        const auto save = [&](auto& field) {
            const size_t n = strlen(event->header_value);
            if (n < sizeof field) memcpy(field, event->header_value, n + 1);
            else field[0] = '\0';
        };
        if (!strcasecmp(event->header_key, "Content-Range")) save(self.contentRange);
        else if (!strcasecmp(event->header_key, "ETag")) save(self.etag);
        else if (!strcasecmp(event->header_key, "Last-Modified")) save(self.modified);
        return ESP_OK;
    }

    const char* validator() const {
        const size_t n = strlen(etag);
        // If-Range requires a strong ETag. Keep its quotes on the wire.
        if (n >= 2 && etag[0] == '"' && etag[n - 1] == '"' && strchr(etag + 1, '"') == etag + n - 1)
            return etag;
        return modified;
    }
};

bool readNumber(const char*& cursor, uint32_t& value, char delimiter) {
    if (*cursor < '0' || *cursor > '9') return false;
    value = 0;
    do {
        value = value * 10 + uint32_t(*cursor++ - '0');
        if (value > resume_detail::kMaxBodyBytes) return false;
    } while (*cursor >= '0' && *cursor <= '9');
    if (*cursor != delimiter) return false;
    cursor += delimiter != '\0';
    return true;
}

bool parseContentRange(const char* cursor, RangeResponse& response) {
    if (strncasecmp(cursor, "bytes ", 6) != 0) return false;
    cursor += 6;
    if (response.status == 416) {
        if (strncmp(cursor, "*/", 2) != 0) return false;
        cursor += 2;
        return readNumber(cursor, response.total, '\0');
    }
    return readNumber(cursor, response.start, '-') && readNumber(cursor, response.end, '/') &&
        readNumber(cursor, response.total, '\0') &&
        response.start <= response.end && response.end < response.total;
}

}  // namespace

HttpsResult httpsGet(const char* url, char* buffer, size_t cap, uint32_t deadlineMs, void (*feed)(),
                     const char* range) {
    HttpsResult result;
    if (!cap || strncmp(url, "https://", 8)) return result;
    buffer[0] = '\0';
    HttpsRequest request(url, deadlineMs, feed);
    const auto client = request.client;
    if (!client || !request.setHeader("Range", range)) return result;
    const int64_t announced = request.fetchHeaders();
    const int status = announced >= 0 ? esp_http_client_get_status_code(client) : -1;
    if (status > 0) {
        result.status = status;
        const auto complete = [&] {
            // fetch_headers returns zero for chunked/unknown length. A known
            // length must count bytes actually copied, including ranged tails.
            return announced > 0 ? uint64_t(announced) == result.length
                                 : esp_http_client_is_complete_data_received(client);
        };
        if (announced > 0 && uint64_t(announced) >= cap) result.truncated = true;
        while (!result.truncated) {
            if (result.length + 1 >= cap) {
                char probe;
                result.truncated = request.read(&probe, 1) > 0;
                break;
            }
            // Read in chunks to check the deadline and feed the watchdog.
            constexpr size_t Chunk = 1024;
            const int got = request.read(buffer + result.length, std::min(Chunk, cap - 1 - result.length));
            if (got <= 0) break;
            result.length += size_t(got);
            if (complete()) break;
        }
        if (!result.truncated && !complete()) result.status = -1;
    }
    buffer[result.length] = '\0';
    return result;
}

HttpsResult httpsGetRange(const char* url, uint32_t start, uint32_t end, const char* ifRange,
                          const ChunkSink& sink, uint32_t deadlineMs, void (*feed)(), RangeResponse& out,
                          const ResponseHeaders& headers) {
    HttpsResult result;
    out = {};
    if (start > end) return result;
    char range[40];
    snprintf(range, sizeof range, "bytes=%" PRIu32 "-%" PRIu32, start, end);

    RangeHeaders received;
    esp_http_client_config_t config{};
    config.user_data = &received;
    config.event_handler = RangeHeaders::collect;
    HttpsRequest request(url, deadlineMs, feed, config);
    const auto client = request.client;
    if (!client || !request.setHeader("Range", range) || !request.setHeader("If-Range", ifRange))
        return result;
    const int64_t announced = request.fetchHeaders();
    if (announced < 0 || (out.status = esp_http_client_get_status_code(client)) <= 0) return result;
    strcpy(out.validator, received.validator());

    size_t expected = 0;
    if (out.status == 206 || out.status == 416) {
        if (!parseContentRange(received.contentRange, out)) {
            result.truncated = true;
            return result;
        }
        if (out.status == 206) {
            expected = out.end - out.start + 1;
            if (expected > kResumeChunk || (announced > 0 && uint64_t(announced) != expected)) {
                result.truncated = true;
                return result;
            }
        }
    } else if (out.status == 200) {
        if (announced > resume_detail::kMaxBodyBytes) {
            result.truncated = true;
            return result;
        }
        out.total = uint32_t(announced);
        expected = out.total;
    }
    if (!headers(out)) return result;
    if (out.status != 200 && out.status != 206) {
        result.status = out.status;
        return result;
    }

    const size_t limit = expected ? expected : resume_detail::kMaxBodyBytes;
    const auto complete = [&] {
        return announced > 0 ? result.length == expected
                             : esp_http_client_is_complete_data_received(client);
    };
    char packet[1024];
    while (!complete()) {
        const size_t remaining = limit - result.length;
        const int count = request.read(packet, std::min(sizeof packet, remaining + 1));
        if (count <= 0) {
            if (count == 0 && complete()) break;
            return result;
        }
        if (size_t(count) > remaining) {
            result.truncated = true;
            return result;
        }
        result.length += size_t(count);
        if (!sink(reinterpret_cast<const uint8_t*>(packet), size_t(count))) return result;
    }
    if (expected && result.length != expected) return result;
    result.status = out.status;
    return result;
}

HttpsResult httpsGetRange(const char* url, uint32_t start, uint32_t end, const char* ifRange,
                          const ChunkSink& sink, uint32_t deadlineMs, void (*feed)(), RangeResponse& out) {
    return httpsGetRange(url, start, end, ifRange, sink, deadlineMs, feed, out,
                         [](const RangeResponse&) { return true; });
}
