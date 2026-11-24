// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Arduino.h>
#include <cstdint>
#include <cstring>
#include "log.h"
#ifndef VENTANAL_HOST_TEST
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif
namespace lcd {
enum class DiagStage : uint8_t { Wake, Join, Connect, Handshake, Send, Read, Restart, Boot, Reboot, Socket, SocketConnect, SocketSelect, SocketError, TlsSetup };
struct DiagEntry { uint32_t time; int32_t a, b, c, d, e, f; DiagStage type; };
struct DiagStorage { uint32_t magic, next, count, unread; DiagEntry entries[96]; };
#ifndef VENTANAL_HOST_TEST
inline portMUX_TYPE diagMux = portMUX_INITIALIZER_UNLOCKED;
struct DiagLock { DiagLock() { portENTER_CRITICAL(&diagMux); } ~DiagLock() { portEXIT_CRITICAL(&diagMux); } };
#else
struct DiagLock {};
#endif
class DiagRing {
public:
    explicit DiagRing(DiagStorage& storage) : s(storage) {}
    void add(uint32_t t, DiagStage type, int a=0, int b=0, int c=0, int d=0, int e=0, int f=0) {
        DiagLock lock; (void)lock; init();
        s.entries[s.next] = {t,a,b,c,d,e,f,type}; s.next=(s.next+1)%96;
        if(s.count<96) ++s.count; if(s.unread<96) ++s.unread;
    }
    void wake(uint32_t t) { add(t,DiagStage::Wake); }
    void join(uint32_t t,int rssi,int channel,bool up,bool ipv4,bool gateway,bool route,int sockets) {
        add(t,DiagStage::Join,rssi,channel,(int(up)|int(ipv4)<<1|int(gateway)<<2|int(route)<<3),sockets);
    }
    void failure(uint32_t t,DiagStage stage,int err,int freeHeap,int largest) { add(t,stage,err,freeHeap,largest); }
    void socketCreated(uint32_t t, int fd, int err) { add(t, DiagStage::Socket, fd, err, -2, 0); }
    void socketConnected(uint32_t t, int fd, int result, int err) { add(t, DiagStage::SocketConnect, fd, result, err); }
    void socketSelected(uint32_t t, int result, int err, int elapsedMs, int timeoutMs, bool writable) {
        add(t, DiagStage::SocketSelect, result, err, elapsedMs, timeoutMs, int(writable));
    }
    void tlsSetupFailed(uint32_t t, int code, int freeHeap, int largest) { add(t, DiagStage::TlsSetup, code, freeHeap, largest); }
    void socketError(uint32_t t, int fd, int result, int err, int soError) { add(t, DiagStage::SocketError, fd, result, err, soError); }
    void printUnread() {
        for (;;) {
            DiagEntry e{};
            { DiagLock lock; (void)lock; init(); if(!s.unread) break;
              e=s.entries[(s.next+96-s.unread)%96]; --s.unread; }
            switch(e.type) {
            case DiagStage::Wake: log("diag ring t=%u wake\n",unsigned(e.time)); break;
            case DiagStage::Join: log("diag ring t=%u join rssi=%d channel=%d netif_up=%d ipv4=%d gateway=%d route=%d sockets=%d\n",unsigned(e.time),e.a,e.b,e.c&1,(e.c>>1)&1,(e.c>>2)&1,(e.c>>3)&1,e.d); break;
            case DiagStage::Restart: log("diag ring t=%u wifi-restart ok=%d\n",unsigned(e.time),e.a); break;
            case DiagStage::Boot: log("diag ring t=%u boot reason=%d\n",unsigned(e.time),e.a); break;
            case DiagStage::Reboot: log("diag ring t=%u reboot reason=network-stuck\n",unsigned(e.time)); break;
            case DiagStage::Socket: log("diag ring t=%u socket fd=%d socket_errno=%d\n",unsigned(e.time),e.a,e.b); break;
            case DiagStage::SocketConnect: log("diag ring t=%u socket_connect fd=%d result=%d connect_errno=%d\n",unsigned(e.time),e.a,e.b,e.c); break;
            case DiagStage::SocketSelect: log("diag ring t=%u socket_select result=%d select_errno=%d elapsed_ms=%d timeout_ms=%d writable=%d\n",unsigned(e.time),e.a,e.b,e.c,e.d,e.e); break;
            case DiagStage::SocketError: log("diag ring t=%u socket_error fd=%d result=%d getsockopt_errno=%d so_error=%d\n",unsigned(e.time),e.a,e.b,e.c,e.d); break;
            case DiagStage::TlsSetup:
                if (e.a < 0) log("diag ring t=%u tls_setup code=%d (-0x%04X) free_internal=%d largest_internal=%d\n",unsigned(e.time),e.a,0u-unsigned(e.a),e.b,e.c);
                else log("diag ring t=%u tls_setup code=%d free_internal=%d largest_internal=%d\n",unsigned(e.time),e.a,e.b,e.c);
                break;
            default: { const char* stage=e.type==DiagStage::Connect?"connect":e.type==DiagStage::Handshake?"handshake":e.type==DiagStage::Send?"send":"read";
                log("diag ring t=%u failure stage=%s errno=%d free_internal=%d largest_internal=%d\n",unsigned(e.time),stage,e.a,e.b,e.c); }
            }
        }
    }
private:
    void init() { if(s.magic!=0x56445231 || s.next>=96 || s.count>96 || s.unread>s.count) { std::memset(&s,0,sizeof s); s.magic=0x56445231; } }
    DiagStorage& s;
};
#ifndef VENTANAL_HOST_TEST
inline RTC_NOINIT_ATTR DiagStorage diagStorage;
#else
inline DiagStorage diagStorage{};
#endif
inline DiagRing diagRing(diagStorage);
}
