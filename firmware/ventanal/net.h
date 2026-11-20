// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "logic.h"
#include "ota.h"
#include "../common/release_key.h"
#include <cstdio>
#include <ctime>
bool wifiConnect(uint32_t timeoutMs = 15000);
time_t seedRtcTime();
bool syncTime();
struct ScreenInfo { char id[17]; char url[96]; ReadoutRect readout; };
bool fetchManifest(ScreenInfo& art, ScreenInfo& weather, bool* transient = nullptr);

struct ManifestExtras { ScreenInfo solar{}; ScreenInfo spares[3]{}; uint8_t count=0; ScreenInfo extraScreens[2]{}; char names[2][49]{}; Screen order[5]{}; uint8_t orderCount=0; };
inline ManifestExtras manifestExtras;
inline char manifestSkipQuery[280]{};

#ifndef VENTANAL_FIRMWARE_VERSION
#define VENTANAL_FIRMWARE_VERSION "1.0.0"
#endif
inline constexpr char VENTANAL_BOARD_ID[] = "inkplate13spectra";
inline char manifestBoardQuery[256]{};
inline void setBoardReport(int pct, const char* screen, const char* frame, int millivolts=0,
                           const OtaRecord& ota={}, const char* approval="") {
    if(pct<0 || pct>100 || (strcmp(screen,"art") && strcmp(screen,"weather") && strcmp(screen,"solar") && strncmp(screen,"extra-",6))) {
        manifestBoardQuery[0]=0; return;
    }
    bool valid=strlen(frame)==16;
    for(unsigned i=0; valid && i<16; ++i)
        valid=(frame[i]>='0' && frame[i]<='9') || (frame[i]>='a' && frame[i]<='f');
    snprintf(manifestBoardQuery,sizeof manifestBoardQuery,"bat=%d&screen=%s%s%s&fw=%s&board=%s",
             pct,screen,valid?"&frame=":"",valid?frame:"",VENTANAL_FIRMWARE_VERSION,VENTANAL_BOARD_ID);
    if(millivolts>0) {
        const size_t used=strlen(manifestBoardQuery);
        snprintf(manifestBoardQuery+used,sizeof manifestBoardQuery-used,"&bat_mv=%d",millivolts);
    }
    char report[otaReportCapacity];
    formatOtaReport(report,ota);
    if(report[0]) {
        const size_t used=strlen(manifestBoardQuery);
        snprintf(manifestBoardQuery+used,sizeof manifestBoardQuery-used,"&ota=%s",report);
    }
    if((ota.result==OtaResult::Ok || ota.result==OtaResult::RolledBack) && validOtaApproval(approval)) {
        const size_t used=strlen(manifestBoardQuery);
        snprintf(manifestBoardQuery+used,sizeof manifestBoardQuery-used,"&ota_approval=%s",approval);
    }
    if(VENTANAL_TEST_RELEASE_KEY) {
        const size_t used=strlen(manifestBoardQuery);
        snprintf(manifestBoardQuery+used,sizeof manifestBoardQuery-used,"&test_release_key=1");
    }
}
