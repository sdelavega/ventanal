// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../common/resume/resume.h"
#include "host_request.h"
#include "sync.h"
#include <esp_heap_caps.h>
#include <memory>

namespace lcd {

// Range GETs for one staging pass (a refresh's frames or an OTA image). They
// share `connection` when the pass owns one (the refresh worker: manifest and
// frames on one keep-alive connection); otherwise this transport keeps its
// own connection for its lifetime (the OTA staging pass).
class HostRangeTransport : public RangeTransport {
public:
    HostRangeTransport(const char* origin, const char* path, const std::string& key,
                       Watchdog& watchdog, uint32_t passStarted, bool ota = false,
                       const std::function<void(int)>& hostResult = {},
                       HostConnection* connection = nullptr)
        : origin_(origin), path_(path), key_(key), watchdog_(watchdog), passStarted_(passStarted), ota_(ota),
          hostResult_(hostResult),
          own_(connection ? nullptr : new HostConnection), connection_(connection ? *connection : *own_) {}

    uint32_t remainingMs() const {
        const uint32_t elapsed = millis() - passStarted_;
        return elapsed < TRANSFER_BUDGET_MS ? TRANSFER_BUDGET_MS - elapsed : 0;
    }

    const char* failureReason() const override { return ota_ && malformed_ ? reason_ : nullptr; }
    bool malformed() const { return malformed_; }
    size_t bytesRead() const { return bytesRead_; }
    int status() const { return status_; }
    const char* reason() const { return reason_; }

    bool get(const ResumeSource& source, uint32_t start, uint32_t end, const char* ifRange,
             const ResponseHeaders& headers, const ChunkSink& sink) override {
        reason_ = nullptr;
        malformed_ = false;
        status_ = 0;
        framed_ = false;
        if (!remainingMs()) return interrupt("budget");
        const bool complete = receive(source, start, end, ifRange, headers, sink);
        // Keep the connection only after an exactly-framed body.
        connection_.finish(complete && framed_ && (status_ == 200 || status_ == 206));
        return complete;
    }

private:
    bool receive(const ResumeSource& source, uint32_t start, uint32_t end, const char* ifRange,
                 const ResponseHeaders& headers, const ChunkSink& sink) {
        RangeResponse response{};
        const int status = connection_.get(origin_, String(path_), key_,
            [this] { return remainingMs(); },
            [&](HTTPClient& http) {
                HostConnection::setRange(http, start, end, ifRange);
                const uint32_t remaining = remainingMs();
                http.setConnectTimeout(std::min(remaining, uint32_t(5000)));
                http.setTimeout(std::min(remaining, HTTP_TIMEOUT_MS));
            },
            [this] { watchdog_.feed(); });
        if (status == HostConnection::kNotBegun) return interrupt("transport");
        status_ = response.status = status;
        // A failed GET has already closed the connection, so a re-join in
        // the hook never leaves a socket behind.
        if (hostResult_) hostResult_(status_);
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
        auto& http = connection_.http;

        const auto etag = http.header("ETag");
        const char* tag = etag.c_str();
        size_t tagSize = etag.length();
        if (tagSize >= 2 && tag[0] == '"' && tag[tagSize - 1] == '"') {
            ++tag;
            tagSize -= 2;
        }
        // The host's firmware ETag is the signed image's SHA-256.
        if (ota_ && (response.status == 200 || response.status == 206) && tagSize &&
            (tagSize != 64 || strncmp(tag, source.sha256Hex, tagSize))) return reject("validator");
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
            if (!resume_detail::parseContentRange(contentRange.c_str(), response)) return reject("parse");
            if (response.status == 206) {
                bodySize = response.end - response.start + 1;
                if (bodySize > kResumeChunk || (length >= 0 && size_t(length) != bodySize))
                    return reject("size");
            }
        }

        if (!headers(response)) return false;
        if (response.status != 200 && response.status != 206) return true;
        if (!bodySize) return reject("size");
        framed_ = length >= 0;
        return readBody(http, bodySize, framed_, sink);
    }

    bool interrupt(const char* reason) {
        reason_ = reason;
        return false;
    }

    bool reject(const char* reason) {
        malformed_ = true;
        return interrupt(reason);
    }

    // The body stopped arriving: a transport failure while reading.
    bool bodyFailure(const char* reason) {
        connection_.logFailure("read");
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

    bool readBody(HTTPClient& http, size_t missing, bool framed, const ChunkSink& sink) {
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
                if (count < 0) return bodyFailure("short-body");
                if (count > 0) {
                    bytesRead_ += count;
                    missing -= count;
                    if (!missing) {
                        if (framed && stream->available() > 0) return reject("excess");
                        if (!framed && !waitForClose(http, *stream)) return false;
                    }
                    if (!sink(packet, count)) return false;
                    if (!missing) return true;
                    lastProgress = millis();
                    continue;
                }
            }
            if (!http.connected()) return bodyFailure("short-body");
            if (millis() - lastProgress >= BODY_IDLE_MS) return bodyFailure("body-idle");
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
    bool ota_;
    bool framed_ = false;
    std::function<void(int)> hostResult_;
    std::unique_ptr<HostConnection> own_;   // only without a pass connection
    HostConnection& connection_;
#ifdef VENTANAL_LCD_DIAG_PANEL
    bool sampledHeap_ = false;
#endif
};

}  // namespace lcd
