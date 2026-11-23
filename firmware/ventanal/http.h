// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "net.h"
#include "security.h"
#include "../common/resume/resume.h"
#include <HTTPClient.h>
#include <cstdarg>

inline constexpr char USER_AGENT[] = "Ventanal/" VENTANAL_FIRMWARE_VERSION;

inline void logTransferOutcome(const char* format, ...) {
    char line[128];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    Serial.println(line);
}

inline void configureHttp(HTTPClient& http) {
    http.setConnectTimeout(5000);
    http.setTimeout(15000);
    http.setUserAgent(USER_AGENT);
}

class HostRangeTransport : public RangeTransport {
public:
    enum class Asset { Firmware, Frame };

    HostRangeTransport(const char* url, uint32_t started, uint32_t budgetMs, uint32_t idleMs,
                       Asset asset = Asset::Firmware, size_t abortAfter = 0)
        : url_(url), started_(started), budgetMs_(budgetMs), idleMs_(idleMs),
          asset_(asset), abortAfter_(abortAfter) {}

    size_t bytesRead() const { return bytesRead_; }
    int status() const { return status_; }
    bool aborted() const { return abortAfter_ && bytesRead_ >= abortAfter_; }
    bool budgetExpired() const { return millis() - started_ >= budgetMs_; }

    bool get(const ResumeSource& source, uint32_t start, uint32_t end, const char* ifRange,
             const ResponseHeaders& headers, const ChunkSink& sink) override {
        failure_ = nullptr;
        const bool keepAlive = asset_ == Asset::Frame && securityConfigured();
        TransportClient localClient;
        HTTPClient localHttp;
        auto& client = keepAlive ? requestClient(localClient) : localClient;
        auto& http = keepAlive ? requestHttp(localHttp) : localHttp;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const auto elapsed = millis() - started_;
            if (elapsed >= budgetMs_) return false;
            configureHttp(http);
            http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
            http.setConnectTimeout(std::min(uint32_t(budgetMs_ - elapsed), uint32_t(5000)));
            http.setTimeout(std::min(uint32_t(budgetMs_ - elapsed), idleMs_));
            if (!beginTransport(http, client, url_)) return false;
            auto& secure = secureTransportClient(client);
            const bool reused = keepAlive && secure.connected();
            secure.received = 0;
            http.setReuse(keepAlive);
            const char* names[] = {"Content-Range", "ETag"};
            http.collectHeaders(names, 2);
            if (end != kWholeGet) {
                const auto range = "bytes=" + std::to_string(start) + "-" + std::to_string(end);
                http.addHeader("Range", range.c_str());
                if (*ifRange) http.addHeader("If-Range", ifRange);
            }
            bool framed = false;
            const bool complete = receive(http, source, headers, sink, framed);
            const bool retry = attempt == 0 && reused && status_ < 0 && secure.received == 0 &&
                millis() - started_ < budgetMs_ && budgetMs_ - (millis() - started_) >= 10000; // TCP connect and TLS handshake limits
            // Never reuse a connection with unread or incomplete response bytes.
            http.setReuse(keepAlive && complete && framed && (status_ == 200 || status_ == 206));
            http.end();
            if (!retry) return complete;
            secure.stop();
            logTransferOutcome("transport retry=fresh-connection reason=status-%d", -status_);
        }
        return false;
    }

    const char* failureReason() const override { return failure_; }

private:
    bool reject(const char* reason) { failure_ = reason; return false; }

    bool receive(HTTPClient& http, const ResumeSource& source, const ResponseHeaders& headers,
                 const ChunkSink& sink, bool& framed) {
        RangeResponse response{};
        status_ = response.status = http.GET();
        if (response.status <= 0 || response.status == 408 || response.status == 429 ||
            response.status >= 500 || budgetExpired()) return false;

        const auto tag = http.header("ETag");
        if (tag.length() >= sizeof response.validator) return reject("validator");
        if (asset_ == Asset::Firmware && tag.length() && (response.status == 200 || response.status == 206)) {
            const auto expected = std::string("\"") + source.sha256Hex + "\"";
            if (expected != tag.c_str()) return reject("validator");
        }
        memcpy(response.validator, tag.c_str(), tag.length());

        const int length = http.getSize();
        framed = length >= 0;
        size_t bodySize = 0;
        if (response.status == 200) {
            if (framed && length != int(source.expectedSize)) return reject("size");
            bodySize = response.total = source.expectedSize;
        } else if (response.status == 206 || response.status == 416) {
            const auto range = http.header("Content-Range");
            if (!resume_detail::parseContentRange(range.c_str(), response)) return reject("range");
            if (response.status == 206) {
                bodySize = response.end - response.start + 1;
                if (bodySize > kResumeChunk || (framed && length != int(bodySize))) return reject("size");
            }
        }

        if (!headers(response)) return false;
        if (!bodySize) return true;
        auto* stream = http.getStreamPtr();
        if (!stream) return false;
        uint8_t block[1024];
        uint32_t last = millis();
        while (!budgetExpired() && millis() - last < idleMs_) {
            const int available = stream->available();
            if (available > 0) {
                if (!bodySize) return reject("excess");
                auto limit = std::min({size_t(available), bodySize, sizeof block});
                if (abortAfter_) limit = std::min(limit, abortAfter_ - bytesRead_);
                const int count = stream->read(block, limit);
                if (count <= 0) return false;
                bytesRead_ += count;
                if (budgetExpired()) return false;
                bodySize -= count;
                if (!sink(block, count) || aborted()) return false;
                if (!bodySize && framed)
                    return stream->available() > 0 ? reject("excess") : true;
                last = millis();
                continue;
            }
            if (!http.connected()) return bodySize == 0;
            if (!bodySize && millis() - last >= 1000) return false;
            delay(1);
        }
        return false;
    }

    const char* url_;
    uint32_t started_, budgetMs_, idleMs_;
    Asset asset_;
    size_t abortAfter_, bytesRead_ = 0;
    int status_ = 0;
    const char* failure_ = nullptr;
};
