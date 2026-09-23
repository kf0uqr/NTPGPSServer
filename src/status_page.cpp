#include "status_page.h"
#include "config.h"
#include "ntp_server.h"
#include "timekeeper.h"

#include <WebServer.h>
#include <WiFi.h>

const char *sourceName(SyncSource s);  // main.cpp

namespace status_page {
namespace {

WebServer server(80);
const NmeaParser *gps = nullptr;
bool started = false;

String utcString() {
    int64_t sec;
    uint32_t us;
    if (!timekeeper::now(sec, us)) return "not synchronised";
    time_t t = (time_t)sec;
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03u UTC",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec, (unsigned)(us / 1000));
    return buf;
}

String json() {
    const TimeStatus st = timekeeper::status();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"utc\":\"%s\",\"source\":\"%s\",\"synced\":%s,\"holdover\":%s,"
             "\"seconds_since_sync\":%u,\"root_dispersion_s\":%.6f,"
             "\"clock_error_ppm\":%.3f,\"pps_count\":%u,\"satellites\":%u,"
             "\"fix_quality\":%u,\"hdop\":%.1f,\"nmea_sentences\":%u,"
             "\"nmea_checksum_errors\":%u,\"ntp_requests\":%u,\"wifi_rssi\":%d,"
             "\"uptime_s\":%lu,\"rejected_dates\":%u,\"rejected_steps\":%u,"
             "\"accepted_steps\":%u,\"bad_pulses\":%u}",
             utcString().c_str(), sourceName(st.source),
             st.synced ? "true" : "false", st.holdover ? "true" : "false",
             (unsigned)st.sinceSyncS, st.dispersionS, st.freqPpm, (unsigned)st.ppsCount,
             gps->satellites(), gps->fixQuality(), gps->hdop(), (unsigned)gps->sentences(),
             (unsigned)gps->checksumErrors(), (unsigned)ntp_server::requestsServed(),
             (int)WiFi.RSSI(),
             (unsigned long)(millis() / 1000), (unsigned)st.rejectedDates,
             (unsigned)st.rejectedSteps, (unsigned)st.acceptedSteps,
             (unsigned)st.badPulses);
    return buf;
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>GPS NTP server</title>
<style>
body{font-family:system-ui,sans-serif;max-width:32rem;margin:1rem auto;padding:0 1rem}
td{padding:.2rem .6rem}td:first-child{color:#666}
#utc{font:600 1.4rem ui-monospace,monospace}
</style></head><body>
<h2>GPS NTP server</h2><div id="utc">…</div><table id="t"></table>
<script>
async function tick(){
 try{const s=await (await fetch('/status.json')).json();
  document.getElementById('utc').textContent=s.utc;
  document.getElementById('t').innerHTML=Object.entries(s).filter(([k])=>k!='utc')
   .map(([k,v])=>`<tr><td>${k.replace(/_/g,' ')}</td><td>${v}</td></tr>`).join('');
 }catch(e){}
}
tick();setInterval(tick,1000);
</script></body></html>)HTML";

}  // namespace

void begin(const NmeaParser &nmea) {
    gps = &nmea;
    if (started) return;
    server.on("/", [] { server.send_P(200, "text/html", PAGE); });
    server.on("/status.json", [] { server.send(200, "application/json", json()); });
    server.begin();
    started = true;
}

void handle() {
    if (started) server.handleClient();
}

}  // namespace status_page
