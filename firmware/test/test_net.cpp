// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../ventanal/net.h"
#include "../ventanal/battery.h"
#include <HTTPClient.h>
#include <Inkplate.h>
#include <cassert>
#include "../ventanal/quiet.h"
#include <iostream>
Inkplate display;
const std::string screen = R"({"id":"0123456789abcdef","url":"/frames/weather-0123456789abcdef.vnf","size":960016,"readout":{"x":1236,"y":30,"w":334,"h":136,"style":"lcars"}})";
void manifest(const std::string& art, const std::string& weather) {
    responseBody="{\"version\":1,\"screens\":{\"art\":"+art+",\"weather\":"+weather+"}}";
    advertisedSize=responseBody.size();
}
std::string replace(std::string s, const char* a, const char* b) {
    s.replace(s.find(a), std::strlen(a), b); return s;
}
int main() {
    assert(syncTime()); // COMPLETED is consumed by the IDF getter.
    ScreenInfo a{},w{};
    auto art=replace(screen,"lcars","shade");
    art=replace(art,"1236","1020"); art=replace(art,"30","0");
    art=replace(art,"334","580"); art=replace(art,"136","104");
    manifest(art,screen);
    assert(fetchManifest(a,w));
    setBoardReport(95, "art", "0123456789abcdef");
    strcpy(manifestSkipQuery,"0123456789abcdef~fedcba9876543210");
    assert(fetchManifest(a,w));
    assert(lastUrl.find("?bat=95&screen=art&frame=0123456789abcdef&fw=1.0.0&board=inkplate13spectra")!=std::string::npos);
    assert(lastUrl.find("&skips=0123456789abcdef~fedcba9876543210")!=std::string::npos);
    setBoardReport(0, "weather", "");
    manifestSkipQuery[0]=0;
    assert(fetchManifest(a,w));
    assert(lastUrl.find("bat=0&screen=weather&fw=1.0.0")!=std::string::npos);
    assert(lastUrl.find("frame=")==std::string::npos);
    assert(lastUserAgent == "Ventanal/1.0.0");

    OtaRecord result{OtaResult::RolledBack, OtaFailure::None, 0x11, "65535.65535.65535"};
    setBoardReport(50, "weather", "", 3800, result);
    assert(fetchManifest(a,w));
    assert(lastUrl.find("&bat_mv=3800&ota=rolled_back:65535.65535.65535")!=std::string::npos);
    responseBody.insert(1, R"("firmware":{"release":"{\"board\":\"inkplate13spectra\"}","sig":"signed","path":"/firmware/inkplate13spectra-2.0.0.bin"},)");
    advertisedSize=responseBody.size();
    assert(fetchManifest(a,w));
    assert(!strcmp(manifestFirmware.release,R"({"board":"inkplate13spectra"})"));
    assert(!strcmp(manifestFirmware.signature,"signed"));
    assert(!strcmp(manifestFirmware.path,"/firmware/inkplate13spectra-2.0.0.bin"));
    responseBody=replace(responseBody, "signed", R"(sig\u0000ned)");
    advertisedSize=responseBody.size();
    assert(fetchManifest(a,w) && !manifestFirmware.release[0]);
    manifest(art,screen);
    assert(fetchManifest(a,w) && !manifestFirmware.release[0]);
    assert(!a.readout.lcars && w.readout.lcars && w.readout.x==1236);
    assert(a.readout.x==1020 && a.readout.y==0 && a.readout.w==580 && a.readout.h==104);
    assert(std::string(w.id)=="0123456789abcdef");
    responseBody.insert(1, "\"quiet\":{\"start\":\"21:30\",\"end\":\"07:15\"},");
    advertisedSize=responseBody.size();
    assert(fetchManifest(a,w));
    assert(quietHours.start==1290 && quietHours.end==435);
    Preferences prefs; prefs.begin("ventanal",false);
    assert(storeQuiet(prefs,quietHours)); quietHours={}; loadQuiet(prefs);
    assert(quietStart(0)==1290 && quietEnd(0)==435);
    quietHours={}; assert(quietStart(1380)==1380 && quietEnd(360)==360);
    assert(parseMinute("24:00")==-1 && parseMinute("7:00")==-1);
    manifest(art,screen);
    responseBody.insert(1,"\"spares\":["+art+","+art+"],"); advertisedSize=responseBody.size();
    assert(fetchManifest(a,w) && manifestExtras.count==2);
    assert(!strcmp(manifestExtras.spares[0].id,a.id) && !manifestExtras.spares[0].readout.lcars);
    manifest(art,screen); assert(fetchManifest(a,w) && manifestExtras.count==0);
    responseBody.insert(1,"\"spares\":[{}],"); advertisedSize=responseBody.size();
    assert(!fetchManifest(a,w));
    for(const char* start : {"24:00","6:00","12:60"}) {
        manifest(art,screen);
        responseBody.insert(1,std::string("\"quiet\":{\"start\":\"")+start+"\",\"end\":\"06:00\"},");
        advertisedSize=responseBody.size(); assert(!fetchManifest(a,w));
    }
    manifest(art,screen);
    responseBody.insert(1,"\"quiet\":{\"start\":\"12:00\",\"end\":\"12:00\"},");
    advertisedSize=responseBody.size(); assert(fetchManifest(a,w));
    assert(quietHours.start==720 && quietHours.end==720);
    assert(storeQuiet(prefs,quietHours)); quietHours={}; loadQuiet(prefs);
    assert(quietStart(0)==720 && quietEnd(0)==720);
    setBoardReport(95,"art","0123456789abcdef",4144);
    manifest(art,screen); assert(fetchManifest(a,w));
    assert(lastUrl.find("&bat_mv=4144")!=std::string::npos);
    assert(manifestBatteryFullMv==0);
    for(int full : {4000,4144,4300}) {
        manifest(art,screen);
        responseBody.insert(1,"\"battery\":{\"full_mv\":"+std::to_string(full)+"},");
        advertisedSize=responseBody.size(); assert(fetchManifest(a,w));
        assert(manifestBatteryFullMv==full); assert(storeBattery(prefs,full));
    }
    for(const char* bad : {"3999","4301","4144.5","\"4144\"","true","null"}) {
        manifest(art,screen);
        responseBody.insert(1,std::string("\"battery\":{\"full_mv\":")+bad+"},");
        advertisedSize=responseBody.size(); assert(fetchManifest(a,w));
        assert(manifestBatteryFullMv==4300);
    }
    manifest(art,screen); assert(fetchManifest(a,w));
    assert(manifestBatteryFullMv==0); assert(storeBattery(prefs,0));
    manifest(art,screen);
    responseBody=replace(responseBody,"\"weather\":", ("\"solar\":"+screen+",\"weather\":").c_str());
    advertisedSize=responseBody.size(); assert(fetchManifest(a,w));
    assert(!strcmp(manifestExtras.solar.id,"0123456789abcdef"));
    setBoardReport(75,"solar",manifestExtras.solar.id);
    assert(strstr(manifestBoardQuery,"screen=solar"));
    manifest(art,screen); assert(fetchManifest(a,w));
    assert(!manifestExtras.solar.id[0]);
    auto previous=w;
    for (const auto& bad : {
        replace(screen,"0123456789abcdef","bad"),
        replace(screen,"/frames/","http://other/"),
        replace(screen,"/frames/","/frames/../"),
        replace(screen,"960016","960015"),
        replace(screen,"1236","2147483647"),
        replace(screen,"334","0"),
        replace(screen,"lcars","acrylic"),
        replace(screen,"lcars","unknown")}) {
        manifest(screen,bad);
        assert(!fetchManifest(a,w));
        assert(std::memcmp(&w,&previous,sizeof(w))==0);
    }
    manifest(screen,screen);
    responseBody=replace(responseBody,"\"version\":1","\"version\":2");
    assert(!fetchManifest(a,w));
    responseBody="{"; advertisedSize=1;
    assert(!fetchManifest(a,w));
    manifest(screen,screen); advertisedSize=8193;
    assert(!fetchManifest(a,w));
    manifest(screen,screen); status=503;
    assert(!fetchManifest(a,w));
    status=200; manifest(screen,screen);
    readDelay=15000;
    const auto started=fakeClock;
    assert(!fetchManifest(a,w));
    assert(fakeClock-started < 31000);
    std::cout << "manifest validation tests passed\n";
}
