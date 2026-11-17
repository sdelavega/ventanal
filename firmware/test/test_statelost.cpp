// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#define main baseline_main
#include "test_wake.cpp"
#undef main
#include <new>
int main() {
    fakeReset=ESP_RST_POWERON; wake(0);
    State initial{}; Preferences seeded; seeded.begin("ventanal",false);
    seeded.getBytes("state13",&initial,sizeof initial);
    initial.owner.queue.push("aaaaaaaaaaaaaaaa","cccccccccccccccc");
    strcpy(initial.owner.spares[0].id,"cccccccccccccccc");
    seeded.putBytes("state13",&initial,sizeof initial);
    auto good=preferences["state13"];
    // A transient read must retry and retain owner metadata, never quarantine.
    online=false;
    Preferences::stateReadFailures=1;
    const int reads=Preferences::stateReads;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(Preferences::stateReads==reads+2);
    assert(preferences["state13"]==good && !preferences.count("state-bad"));
    online=true;
    // Both incompatible sizes and exhausted reads recover this wake. Each
    // quarantine replaces the previous one and retains the exact original.
    unsigned recoveryCase=0;
    for(const auto& bad : {std::vector<uint8_t>{1,2,3}, std::vector<uint8_t>{}, good, good}) {
        preferences["state13"]=bad;
        if(bad==good) Preferences::stateReadFailures=2;
        const bool retryButton=++recoveryCase==4;
        if(retryButton) {
            rememberPanel(initial,75,false,123456);
            pendingPanel.magic=ponMagic; pendingPanel.attempts=1;
            fakeReset=ESP_RST_SW;
        }
        Serial.output.clear();
        const int beforeConnections=connections, beforeDownloads=downloads;
        const unsigned beforeDraws=display.refreshes;
        wake(retryButton?ESP_SLEEP_WAKEUP_EXT0:ESP_SLEEP_WAKEUP_TIMER);
        fakeReset=ESP_RST_POWERON;
        assert(Serial.output.find("error=state-load quarantined bytes="+std::to_string(bad.size()))!=std::string::npos);
        assert(preferences.count("state-bad") && preferences["state-bad"]==bad);
        assert(preferences["state13"].size()==sizeof(State) && preferences["state13"]!=bad);
        assert(connections==beforeConnections+1 && downloads==beforeDownloads+2);
        assert(display.refreshes==beforeDraws+1);
        const int afterDownloads=downloads;
        wake(ESP_SLEEP_WAKEUP_TIMER);
        assert(downloads==afterDownloads); // Fresh record loads normally next wake.
    }
    // If even the raw quarantine read fails, preserve the only surviving copy.
    preferences["state13"]=good;
    failRawStateRead=true;
    Preferences::failStateRead=true;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(preferences["state13"]==good);
    Preferences::failStateRead=false;
    failRawStateRead=false;
    // Persistent Preferences errors can still salvage the raw blob.
    Preferences::failStateRead=true;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(preferences["state-bad"]==good && preferences["state13"]!=good);
    Preferences::failStateRead=false;
    // A quarantine write failure must also retain the sole durable State.
    preferences["state13"]={1,2,3};
    failQuarantineWrite=true;
    const auto priorQuarantine=preferences["state-bad"];
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert((preferences["state13"]==std::vector<uint8_t>{1,2,3}));
    assert(preferences["state-bad"]==priorQuarantine);
    failQuarantineWrite=false;
    preferences["state13"]=good;
    State state{};
    state.owner.cursor=2;
    strcpy(state.owner.spares[0].id,"cccccccccccccccc");
    strcpy(state.owner.overrideArt.id,"dddddddddddddddd");
    state.owner.queue.push("aaaaaaaaaaaaaaaa","dddddddddddddddd");
    rememberPanel(state,75,false,123456);
    pendingPanel.magic=ponMagic; pendingPanel.attempts=1;
    // C++ startup default-initializes global objects even in RTC NOINIT.
    // Replay that initialization on retained bytes, unlike the old reset fake.
    new (&pendingPanel) PendingPanel;
    assert(pendingPanel.magic==ponMagic && pendingPanel.attempts==1);
    assert(panelState().owner.queue.count==1);
    assert(panelState().owner.cursor==2);
    assert(!strcmp(panelState().owner.spares[0].id,"cccccccccccccccc"));
    assert(!strcmp(panelState().owner.overrideArt.id,"dddddddddddddddd"));
    // Actual application sequence, including startup initialization on retry.
    manifestExtras.count=3;
    for(int i=0;i<3;++i) {
        manifestExtras.spares[i]=art;
        memset(manifestExtras.spares[i].id,'c'+i,16);
    }
    fakeReset=ESP_RST_POWERON; wake(0);
    auto load=[] {
        State loaded{}; Preferences prefs; prefs.begin("ventanal",false);
        assert(prefs.getBytes("state13",&loaded,sizeof loaded)==sizeof loaded);
        return loaded;
    };
    auto twice=[] {
        const auto base=fakeClock;
        digitalReader=[base](int pin) {
            if(pin!=18) return fakeBusy;
            const auto elapsed=fakeClock-base;
            return elapsed<80 || (elapsed>=250 && elapsed<350)?LOW:HIGH;
        };
    };
    const int networkBefore=connections;
    twice(); fakeWake=ESP_SLEEP_WAKEUP_EXT0;
    displayHook=[] { fakeBusy=0; fakeClock=panelStarted+3000; timerCallback(nullptr); };
    try { setup(); assert(false); } catch(const Restarting&) {}
    auto selected=load();
    assert(selected.owner.queue.count==1 && selected.owner.cursor==0);
    for(int retry=1;retry<=2;++retry) {
        new (&pendingPanel) PendingPanel;
        assert(panelState().owner.queue.count==1);
        fakeReset=ESP_RST_SW; fakeClock=0; digitalReader={};
        if(retry==1) {
            try { setup(); assert(false); } catch(const Restarting&) {}
        } else {
            displayHook={}; fakeBusy=1; wake(0);
        }
    }
    assert(connections==networkBefore);
    auto retried=load();
    assert(!memcmp(&retried.owner,&selected.owner,sizeof selected.owner));
    // Further button selections survive both watchdog recovery and its limit.
    twice(); fakeReset=ESP_RST_DEEPSLEEP; wake(ESP_SLEEP_WAKEUP_EXT0);
    selected=load(); assert(selected.owner.queue.count==2 && selected.owner.cursor==1);
    online=false; digitalReader={}; fakeReset=ESP_RST_TASK_WDT;
    wake(0);
    auto recovered=load();
    assert(!memcmp(&recovered.owner,&selected.owner,sizeof selected.owner));
    uint8_t strikes=2; Preferences prefs; prefs.begin("ventanal",false);
    prefs.putBytes("wdt-count",&strikes,1);
    const auto bytes=preferences["state13"];
    wake(0);
    assert(preferences["state13"]==bytes);
    // Retry limit sleeps without replacing the pre-display persisted selection.
    rememberPanel(selected,75,false,123456);
    pendingPanel.attempts=2;
    displayHook=[] { fakeBusy=0; fakeClock=panelStarted+3000; timerCallback(nullptr); };
    try { refreshPanel(); assert(false); } catch(const Sleeping&) {}
    assert(preferences["state13"]==bytes);
    Serial.output.clear();
    try { sleepFor(60,selected); assert(false); } catch(const Sleeping&) {}
    assert(Serial.output.find("state phase=sleep")!=std::string::npos);
    assert(Serial.output.find("queue=2 cursor=1")!=std::string::npos);
    displayHook={}; fakeBusy=1; online=true; fakeReset=ESP_RST_DEEPSLEEP;
    const int beforeDownloads=downloads;
    wake(ESP_SLEEP_WAKEUP_TIMER);
    assert(downloads==beforeDownloads && load().owner.queue.count==0);
    assert(!strcmp(manifestSkipQuery,"aaaaaaaaaaaaaaaa~cccccccccccccccc,cccccccccccccccc~dddddddddddddddd"));
    // A corrupt RTC snapshot must fall back to the durable NVS record.
    rememberPanel(load(),75,false,123456);
    pendingPanel.magic=ponMagic; pendingPanel.attempts=1;
    pendingPanel.stateBytes[0]^=1;
    fakeReset=ESP_RST_SW;
    const auto beforeCorrupt=connections;
    wake(0);
    assert(connections==beforeCorrupt+1 && downloads==beforeDownloads);
    puts("statelost: startup/retry round-trip, read retry/quarantine/recovery, explicit sleep logging, repeated skips, watchdog/limits, RTC corruption and unchanged spare cache passed");
}
