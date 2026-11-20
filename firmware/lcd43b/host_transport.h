// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../common/resume/resume.h"
#include "host_request.h"
#include "sync.h"
#include <cerrno>
#include <cstdlib>
#include <strings.h>
#ifdef VENTANAL_LCD_DIAG_PANEL
#include <esp_heap_caps.h>
#endif

namespace lcd {

class HostRangeTransport : public RangeTransport {
public:
    HostRangeTransport(const char* origin, const char* path, const std::string& key,
                       Watchdog& watchdog, uint32_t passStarted)
        : origin_(origin), path_(path), key_(key), watchdog_(watchdog), passStarted_(passStarted) {}

    uint32_t remainingMs() const {
        const uint32_t elapsed = millis() - passStarted_;
        return elapsed < TRANSFER_BUDGET_MS ? TRANSFER_BUDGET_MS - elapsed : 0;
    }

    bool malformed() const { return malformed_; }
    size_t bytesRead() const { return bytesRead_; }
    int status() const { return status_; }
    const char* reason() const { return reason_; }

    bool get(const ResumeSource& source, uint32_t start, uint32_t end, const char* ifRange,
             const ResponseHeaders& headers, const ChunkSink& sink) override {
        reason_ = nullptr;
        status_ = 0;
        if (!remainingMs()) return interrupt("budget");
        HostRequest request;
        if (!request.begin(origin_, String(path_), key_)) return interrupt("transport");
        request.setRange(start, end, ifRange);
        auto& http = request.http;
        const uint32_t remaining = remainingMs();
        if (!remaining) return interrupt("budget");
        http.setConnectTimeout(std::min(remaining, uint32_t(5000)));
        http.setTimeout(std::min(remaining, HTTP_TIMEOUT_MS));
        watchdog_.feed();
        RangeResponse response{};
        status_ = response.status = http.GET();
        watchdog_.feed();
#ifdef VENTANAL_LCD_DIAG_PANEL
        if (!sampledHeap_) {
            constexpr auto internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            log("panel heap free_internal=%u largest_internal=%u\n",
                unsigned(heap_caps_get_free_size(internal)),
                unsigned(heap_caps_get_largest_free_block(internal)));
            sampledHeap_ = true;
        }
#endif
        if (!remainingMs()) return interrupt("budget");
        if (response.status == HTTPC_ERROR_READ_TIMEOUT) return interrupt("header-timeout");
        if (response.status <= 0) return interrupt("transport");
        if (response.status == 408 || response.status == 429 || response.status >= 500)
            return false;

        const auto etag = http.header("ETag");
        const char* tag = etag.c_str();
        size_t tagSize = etag.length();
        if (tagSize >= 2 && tag[0] == '"' && tag[tagSize - 1] == '"') {
            ++tag;
            tagSize -= 2;
        }
        if (tagSize >= sizeof response.validator) return reject("parse");
        memcpy(response.validator, tag, tagSize);

        const int length = http.getSize();
        size_t bodySize = 0;
        if (response.status == 200) {
            if (length == 0) return reject("size");
            response.total = length >= 0 ? uint32_t(length) : 0;
            bodySize = response.total ? response.total : source.expectedSize;
        } else if (response.status == 206 || response.status == 416) {
            const auto contentRange = http.header("Content-Range");
            if (!parseContentRange(contentRange.c_str(), response)) return reject("parse");
            if (response.status == 206) {
                bodySize = response.end - response.start + 1;
                if (bodySize > kResumeChunk || (length >= 0 && size_t(length) != bodySize))
                    return reject("size");
            }
        }

        if (!headers(response)) return false;
        if (response.status != 200 && response.status != 206) return true;
        if (!bodySize) return reject("size");
        return readBody(http, bodySize, sink);
    }

private:
    static bool readNumber(const char*& text, uint32_t& number, char delimiter) {
        if (*text < '0' || *text > '9') return false;
        char* end;
        errno = 0;
        const auto value = std::strtoul(text, &end, 10);
        if (errno == ERANGE || value > resume_detail::kMaxBodyBytes || *end != delimiter)
            return false;
        number = value;
        text = end + (delimiter != '\0');
        return true;
    }

    static bool parseContentRange(const char* text, RangeResponse& response) {
        if (strncasecmp(text, "bytes ", 6) != 0) return false;
        text += 6;
        if (response.status == 416) {
            if (strncmp(text, "*/", 2) != 0) return false;
            text += 2;
            return readNumber(text, response.total, '\0');
        }
        return readNumber(text, response.start, '-') && readNumber(text, response.end, '/') &&
            readNumber(text, response.total, '\0') &&
            response.start <= response.end && response.end < response.total;
    }

    bool interrupt(const char* reason) {
        reason_ = reason;
        return false;
    }

    bool reject(const char* reason) {
        malformed_ = true;
        return interrupt(reason);
    }

    bool waitForClose(HTTPClient& http, WiFiClient& stream) {
        const uint32_t started = millis();
        while (remainingMs()) {
            watchdog_.feed();
            if (stream.available() > 0) return reject("excess");
            if (!http.connected()) return true;
            if (millis() - started >= FRAME_CLOSE_GRACE_MS) return interrupt("close-timeout");
            delay(2);
        }
        return interrupt("budget");
    }

    bool readBody(HTTPClient& http, size_t missing, const ChunkSink& sink) {
        auto* stream = http.getStreamPtr();
        if (!stream) return interrupt("short-body");
        uint8_t packet[1024];
        uint32_t lastProgress = millis();
        while (remainingMs()) {
            watchdog_.feed();
            const int available = stream->available();
            if (available > 0) {
                const size_t limit = std::min({size_t(available), missing, sizeof packet});
                const int count = stream->read(packet, limit);
                if (count < 0) return interrupt("short-body");
                if (count > 0) {
                    bytesRead_ += count;
                    missing -= count;
                    // Keep the last bytes out of the partial until closure
                    // proves the body had no delayed trailing bytes.
                    if (!missing && !waitForClose(http, *stream)) return false;
                    if (!sink(packet, count)) return false;
                    if (!missing) return true;
                    lastProgress = millis();
                    continue;
                }
            }
            if (!http.connected()) return interrupt("short-body");
            if (millis() - lastProgress >= BODY_IDLE_MS) return interrupt("body-idle");
            delay(2);
        }
        return interrupt("budget");
    }

    const char* origin_;
    const char* path_;
    const std::string& key_;
    Watchdog& watchdog_;
    uint32_t passStarted_;
    size_t bytesRead_ = 0;
    int status_ = 0;
    const char* reason_ = nullptr;
    bool malformed_ = false;
#ifdef VENTANAL_LCD_DIAG_PANEL
    bool sampledHeap_ = false;
#endif
};

}  // namespace lcd
