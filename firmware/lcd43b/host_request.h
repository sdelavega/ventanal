// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include "../common/resume/resume.h"
#include "../common/handshake_timeout.h"

#include "build_config.h"
#include "../common/release_key.h"
#include "installation.h"
#include "transport.h"
#include "log.h"
#include "diag_ring.h"
#include <cerrno>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <cctype>
#ifndef VENTANAL_HOST_TEST
#include <WiFiClientSecure.h>
#include <mbedtls/x509_crt.h>
#endif

namespace lcd {

using ::handshakeTimeoutSeconds;

inline std::string hostAssetUrl(const char* origin, const char* path) {
    size_t length = strlen(origin);
    while (length && origin[length - 1] == '/') --length;
    return std::string(origin, length) + path;
}

#ifndef VENTANAL_HOST_TEST
void beginSocketDiagnostics();
void endSocketDiagnostics();
class PinnedLeafClient : public WiFiClientSecure {
public:
    void setPins(const char* pem) {
        pins_ = pem;
        setCACert(pem);
    }

    // True when the last connect reached TCP but failed the TLS handshake
    // or the pin check (diagnostics only).
    bool handshakeFailed() const { return handshakeFailed_; }

    int connect(const char* host, uint16_t port, int32_t timeout) override {
        handshakeFailed_ = false;
        if (!pins_) return 0;
        // TCP first, then the same handshake connect() would run, so a
        // failure names its stage.
        setPlainStart();
        beginSocketDiagnostics();
        const int connected = WiFiClientSecure::connect(host, port, timeout);
        endSocketDiagnostics();
        if (!connected) {
            // The plain start still runs the TLS setup (socket options, RNG
            // seed, CA parse, buffers); its error code says which step failed.
            char text[2];
            constexpr auto internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            diagRing.tlsSetupFailed(millis(), lastError(text, sizeof text),
                int(heap_caps_get_free_size(internal)), int(heap_caps_get_largest_free_block(internal)));
            return 0;
        }
        if (!startTLS()) {
            handshakeFailed_ = true;
            return 0;
        }
        const auto* peer = getPeerCertificate();
        mbedtls_x509_crt pins;
        mbedtls_x509_crt_init(&pins);
        bool matched = false;
        if (peer && mbedtls_x509_crt_parse(&pins,
                reinterpret_cast<const unsigned char*>(pins_), strlen(pins_) + 1) == 0) {
            for (auto* pin = &pins; pin && !matched; pin = pin->next) {
                matched = peer->raw.len == pin->raw.len && pin->raw.len &&
                    memcmp(peer->raw.p, pin->raw.p, pin->raw.len) == 0;
            }
        }
        mbedtls_x509_crt_free(&pins);
        if (!matched) {
            handshakeFailed_ = true;
            stop();
        }
        return matched;
    }

private:
    const char* pins_ = nullptr;
    bool handshakeFailed_ = false;
};
#endif

// Counts response bytes the HTTP client consumes, including header bytes
// read inside GET(), so a failed request knows whether a response began.
template<class Base>
struct CountingClient : Base {
    size_t received = 0;
    int read() override {
        const int value = Base::read();
        if (value >= 0) ++received;
        return value;
    }
    int read(uint8_t* buffer, size_t size) override {
        const int count = Base::read(buffer, size);
        if (count > 0) received += size_t(count);
        return count;
    }
};

// One host connection for a refresh pass (or an OTA staging pass). Every
// request in the pass goes over it with HTTP/1.1 keep-alive; the owner
// calls finish() after each response and the connection stays open only
// when that response was read exactly to its Content-Length. Any error, an
// unframed body, excess bytes or a 'Connection: close' answer closes it, and
// the next request opens a fresh one. Each request is signed with its own
// counter and proof, retries included. Not thread-safe: the refresh worker
// (or the OTA stager) is its only user.
class HostConnection {
    // Declared before http: ~HTTPClient stops its client.
    CountingClient<WiFiClient> plain_;
#ifndef VENTANAL_HOST_TEST
    CountingClient<PinnedLeafClient> tls_;
#endif

public:
    // GET() was not attempted: the request could not be prepared locally
    // (unpaired, bad key, plain HTTP outside a test build). Not a host failure.
    static constexpr int kNotBegun = -1000;
    // A stale keep-alive connection is retried on a fresh one only when a
    // full TCP connect and TLS handshake still fit in the budget.
    static constexpr uint32_t kFreshRetryMs = 10000;

    HostConnection() = default;
    HostConnection(const HostConnection&) = delete;
    HostConnection& operator=(const HostConnection&) = delete;
    ~HostConnection() { close(); }

    HTTPClient http;

    // Prepares the next request on this connection; true when it may be sent.
    bool begin(const char* origin, const String& path, const std::string& key, uint32_t remainingMs) {
        const String url(hostAssetUrl(origin, path.c_str()).c_str());
        const bool secured = strncmp(origin, "https://", 8) == 0;
        if (secured) {
            if (!installation.certificate[0] || key.size() != 32) return false;
        } else if (!VENTANAL_TEST_RELEASE_KEY || strncmp(origin, "http://", 7) != 0 || installation.certificate[0]
                   || installation.deviceKey[0] || installation.pairing[0]) {
            return false;
        }
        // Never carry a connection over to a different origin.
        if (open_ && origin_ != hostAssetUrl(origin, "")) close();
        if (open_ && millis() - lastFinished_ >= 8000) {
            log("transport reconnect reason=idle idle_ms=%u\n", unsigned(millis() - lastFinished_));
            close();
        }
        if (secured && !handshakeTimeoutSeconds(remainingMs)) return false;
        auto& client = clientFor(secured);
        reused_ = open_ && client.connected();
        if (open_ && !reused_) close();
        receivedFor(secured) = 0;
#ifndef VENTANAL_HOST_TEST
        if (secured) {
            // The trust bundle contains the current self-signed leaf and,
            // during rotation, may also contain the previous leaf.
            tls_.setPins(installation.certificate);
            tls_.setHandshakeTimeout(handshakeTimeoutSeconds(remainingMs));
        }
#endif
        if (!http.begin(client, url)) return false;
        origin_ = hostAssetUrl(origin, "");
        secured_ = secured;
        open_ = true;
        if (secured && !addPairedHeaders(http, key, installation.pairing, path.c_str())) return false;
        http.setReuse(true);
        http.setTimeout(10000);
        http.setConnectTimeout(5000);
        http.setUserAgent(USER_AGENT);
        const char* headers[] = {"Content-Range", "ETag", "Connection"};
        http.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));
        return true;
    }

    static void setRange(HTTPClient& http, uint32_t start, uint32_t end, const char* ifRange) {
        if (end == kWholeGet) return;
        const auto range = "bytes=" + std::to_string(start) + "-" + std::to_string(end);
        http.addHeader("Range", range.c_str());
        if (*ifRange) http.addHeader("If-Range", ("\"" + std::string(ifRange) + "\"").c_str());
    }

    // Sends one GET: begin(), then prepare(http) for per-request headers and
    // timeouts. A reused connection that fails before any response byte is
    // retried once on a fresh connection (with a new signature). Returns the
    // HTTP status, kNotBegun, or a transport error (<= 0; then closed).
    template<class Remaining, class Prepare, class Feed>
    int get(const char* origin, const String& path, const std::string& key,
            Remaining remainingMs, Prepare prepare, Feed feed) {
        for (unsigned attempt = 0;; ++attempt) {
            if (!begin(origin, path, key, remainingMs())) {
                close();
                return kNotBegun;
            }
            prepare(http);
            feed();
            errno = 0;
            const int status = http.GET();
            const int requestErrno = errno;
            feed();
            if (status > 0) return status;
            // A refused connect means the kept socket was already gone and a
            // fresh connect failed: that attempt was not a reuse.
            const bool wasReused = reused_ && status != HTTPC_ERROR_CONNECTION_REFUSED;
            const bool retry = attempt == 0 && wasReused && !received() &&
                remainingMs() >= kFreshRetryMs;
            logFailure(stageOf(status), wasReused, requestErrno);
            close();
            if (!retry) return status;
            log("transport retry=fresh-connection reason=status-%d\n", -status);
        }
    }

    // After a response: keep the connection only for an exactly-read
    // (framed) body the server did not ask to close.
    void finish(bool keep) {
        if (!open_) return;
        keep = keep && !closeRequested();
        http.setReuse(keep);
        http.end();
        if (!keep) close();
        else lastFinished_ = millis();
    }

    void close() {
        http.setReuse(false);
        http.end();
        plain_.stop();
#ifndef VENTANAL_HOST_TEST
        tls_.stop();
#endif
        open_ = false;
        reused_ = false;
    }

    bool reused() const { return reused_; }

    // Response bytes consumed since the last begin().
    size_t received() { return receivedFor(secured_); }

    // `transport fail stage=<connect|handshake|send|read> free_internal=<b>
    // largest_internal=<b> reused=<0|1>`: heap only, never an address.
    void logFailure(const char* stage) { logFailure(stage, reused_); }

private:
    // The host build has no TLS: secured requests use the plain fake.
    WiFiClient& clientFor(bool secured) {
#ifndef VENTANAL_HOST_TEST
        if (secured) return tls_;
#endif
        (void)secured;
        return plain_;
    }
    size_t& receivedFor(bool secured) {
#ifndef VENTANAL_HOST_TEST
        if (secured) return tls_.received;
#endif
        (void)secured;
        return plain_.received;
    }

    void logFailure(const char* stage, bool reused, int error = 0) {
        const DiagStage kind = !strcmp(stage,"connect") ? DiagStage::Connect : !strcmp(stage,"handshake") ? DiagStage::Handshake : !strcmp(stage,"send") ? DiagStage::Send : DiagStage::Read;
        diagRing.failure(millis(), kind, error, int(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), int(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        constexpr auto internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        log("transport fail stage=%s free_internal=%u largest_internal=%u reused=%d\n", stage,
            unsigned(heap_caps_get_free_size(internal)),
            unsigned(heap_caps_get_largest_free_block(internal)), int(reused));
    }

    const char* stageOf(int status) const {
        if (status == HTTPC_ERROR_CONNECTION_REFUSED) {
#ifndef VENTANAL_HOST_TEST
            if (secured_ && tls_.handshakeFailed()) return "handshake";
#endif
            return "connect";
        }
        if (status == HTTPC_ERROR_SEND_HEADER_FAILED || status == HTTPC_ERROR_SEND_PAYLOAD_FAILED)
            return "send";
        return "read";
    }

    bool closeRequested() {
        const auto value = http.header("Connection");
        std::string lower(value.c_str());
        for (auto& c : lower) c = char(tolower(static_cast<unsigned char>(c)));
        return lower.find("close") != std::string::npos;
    }

    uint32_t lastFinished_ = 0;
    std::string origin_;
    bool open_ = false, reused_ = false, secured_ = false;
};

}  // namespace lcd
