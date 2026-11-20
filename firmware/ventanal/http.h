// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "net.h"
#include "security.h"
#include "../common/resume/resume.h"
#include <HTTPClient.h>

inline constexpr char USER_AGENT[] = "Ventanal/" VENTANAL_FIRMWARE_VERSION;

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
        const auto elapsed = millis() - started_;
        if (elapsed >= budgetMs_) return false;
        TransportClient localClient;
        HTTPClient localHttp;
        const bool keepAlive = asset_ == Asset::Frame && securityConfigured();
        auto& client = keepAlive ? requestClient(localClient) : localClient;
        auto& http = keepAlive ? requestHttp(localHttp) : localHttp;
        configureHttp(http);
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        http.setConnectTimeout(std::min(uint32_t(budgetMs_ - elapsed), uint32_t(5000)));
        http.setTimeout(std::min(uint32_t(budgetMs_ - elapsed), idleMs_));
        if (!beginTransport(http, client, url_)) return false;
        http.setReuse(keepAlive);
        const char* names[] = {"Content-Range", "ETag"};
        http.collectHeaders(names, 2);
        const auto range = "bytes=" + std::to_string(start) + "-" + std::to_string(end);
        http.addHeader("Range", range.c_str());
        if (*ifRange) http.addHeader("If-Range", ifRange);
        const bool complete = receive(http, source, headers, sink);
        // Closing after a timeout or rejected header prevents unread bytes
        // from carrying over to the next authenticated request.
        http.setReuse(keepAlive && complete && (status_ == 200 || status_ == 206));
        http.end();
        return complete;
    }

    const char* failureReason() const override { return failure_; }

private:
    bool reject(const char* reason) { failure_ = reason; return false; }

    bool receive(HTTPClient& http, const ResumeSource& source, const ResponseHeaders& headers,
                 const ChunkSink& sink) {
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
        size_t bodySize = 0;
        if (response.status == 200) {
            if (length != int(source.expectedSize)) return reject("size");
            bodySize = response.total = length;
        } else if (response.status == 206 || response.status == 416) {
            const auto range = http.header("Content-Range");
            if (!resume_detail::parseContentRange(range.c_str(), response)) return reject("range");
            if (response.status == 206) {
                bodySize = response.end - response.start + 1;
                if (bodySize > kResumeChunk || length != int(bodySize)) return reject("size");
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
            // For frame requests Content-Length identifies the body boundary
            // even when the server keeps the connection open.
            if (!bodySize && asset_ == Asset::Frame)
                return available <= 0 ? true : reject("excess");
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
