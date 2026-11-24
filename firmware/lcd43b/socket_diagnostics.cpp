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
