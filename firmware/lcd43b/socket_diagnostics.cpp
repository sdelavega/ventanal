// SPDX-License-Identifier: GPL-3.0-or-later
// Observe the actual TLS TCP calls without recording destination addresses.
#include "diag_ring.h"
#include <atomic>
#include <cerrno>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
std::atomic<TaskHandle_t> observedTask{nullptr};
bool observing() { return observedTask.load() == xTaskGetCurrentTaskHandle(); }
}
namespace lcd {
void beginSocketDiagnostics() { observedTask.store(xTaskGetCurrentTaskHandle()); }
void endSocketDiagnostics() { observedTask.store(nullptr); }
}
extern "C" int __real_lwip_socket(int, int, int);
extern "C" int __real_lwip_connect(int, const struct sockaddr*, socklen_t);
extern "C" int __real_lwip_select(int, fd_set*, fd_set*, fd_set*, struct timeval*);
extern "C" int __real_lwip_getsockopt(int, int, int, void*, socklen_t*);
extern "C" int __wrap_lwip_socket(int domain, int type, int protocol) {
    const int result = __real_lwip_socket(domain, type, protocol);
    const int error = result < 0 ? errno : 0;
    if (observing()) lcd::diagRing.socketCreated(millis(), result, error);
    if (result < 0) errno = error;
    return result;
}
extern "C" int __wrap_lwip_connect(int fd, const struct sockaddr* addr, socklen_t len) {
    const int result = __real_lwip_connect(fd, addr, len);
    const int error = result < 0 ? errno : 0;
    if (observing()) lcd::diagRing.socketConnected(millis(), fd, result, error);
    if (result < 0) errno = error;
    return result;
}
// The TLS client waits for the non-blocking connect with select(); record what
// that wait returned and how long it took, so a fast failure shows its cause.
extern "C" int __wrap_lwip_select(int maxfdp1, fd_set* readset, fd_set* writeset, fd_set* exceptset,
                                  struct timeval* timeout) {
    const bool observed = observing();
    const int fd = maxfdp1 - 1;
    const bool watched = observed && writeset && fd >= 0 && FD_ISSET(fd, writeset);
    const int timeoutMs = timeout ? int(timeout->tv_sec * 1000 + timeout->tv_usec / 1000) : -1;
    const uint32_t start = millis();
    const int result = __real_lwip_select(maxfdp1, readset, writeset, exceptset, timeout);
    const int error = result < 0 ? errno : 0;
    if (watched)
        lcd::diagRing.socketSelected(millis(), result, error, int(millis() - start), timeoutMs,
                                     result > 0 && FD_ISSET(fd, writeset));
    if (result < 0) errno = error;
    return result;
}
extern "C" int __wrap_lwip_getsockopt(int fd, int level, int name, void* value, socklen_t* len) {
    const int result = __real_lwip_getsockopt(fd, level, name, value, len);
    const int error = result < 0 ? errno : 0;
    if (observing() && level == SOL_SOCKET && name == SO_ERROR)
        lcd::diagRing.socketError(millis(), fd, result, error, result == 0 && value ? *static_cast<int*>(value) : 0);
    if (result < 0) errno = error;
    return result;
}
