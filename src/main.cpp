// ESP32 + GPS stratum-1 NTP server.
// See README.md for wiring and setup.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_timer.h>

#include "config.h"
#include "nmea.h"
#include "ntp_server.h"
#include "status_page.h"
#include "timekeeper.h"

static NmeaParser nmea;
static HardwareSerial &gpsSerial = Serial2;
static bool networkUp = false;

const char *sourceName(SyncSource s) {
    switch (s) {
        case SyncSource::Pps:  return "PPS";
        case SyncSource::Nmea: return "NMEA";
        default:               return "none";
    }
}

static void onNetworkUp() {
    Serial.printf("WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
    if (ntp_server::begin()) Serial.println("NTP server listening on UDP 123");
    else Serial.println("ERROR: could not open UDP port 123");
    if (MDNS.begin(HOSTNAME)) MDNS.addService("ntp", "udp", 123);
#if ENABLE_STATUS_PAGE
    status_page::begin(nmea);
#endif
}

// Send GPS_INIT_COMMANDS. Done a couple of times after boot in case the GPS
// was still starting up the first time.
static void sendGpsInit() {
    static const uint32_t SEND_AT_MS[] = {1500, 5000};
    static size_t next = 0;
    if (sizeof(GPS_INIT_COMMANDS) <= 1 || next >= sizeof(SEND_AT_MS) / sizeof(SEND_AT_MS[0]))
        return;
    if (millis() < SEND_AT_MS[next]) return;
    gpsSerial.print(GPS_INIT_COMMANDS);
    Serial.printf("Sent GPS init: %s", GPS_INIT_COMMANDS);
    next++;
}

static void printStatus() {
    const TimeStatus st = timekeeper::status();
    int64_t sec;
    uint32_t us;
    char when[32] = "--";
    if (timekeeper::now(sec, us)) {
        time_t t = (time_t)sec;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    }
    Serial.printf(
        "[%s UTC] src=%s synced=%d holdover=%d age=%us sats=%u fixq=%u "
        "pps=%u freq=%+.2fppm ntp_reqs=%u nmea=%u cserr=%u "
        "bad_dates=%u bad_steps=%u bad_pulses=%u\n",
        when, sourceName(st.source), st.synced, st.holdover, (unsigned)st.sinceSyncS,
        nmea.satellites(), nmea.fixQuality(), (unsigned)st.ppsCount, st.freqPpm,
        (unsigned)ntp_server::requestsServed(), (unsigned)nmea.sentences(),
        (unsigned)nmea.checksumErrors(), (unsigned)st.rejectedDates,
        (unsigned)st.rejectedSteps, (unsigned)st.badPulses);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nESP32 GPS NTP server starting");

    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    // Hand every byte over immediately instead of batching ~120 bytes in the
    // UART FIFO, so NMEA-only timing (no PPS) isn't delayed by the batching.
    gpsSerial.setRxFIFOFull(1);
    timekeeper::begin(PPS_PIN, PPS_RISING_EDGE);

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    // Modem power save adds tens to hundreds of ms of random latency.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
    while (gpsSerial.available()) {
        const char c = (char)gpsSerial.read();
        if (nmea.feed(c, esp_timer_get_time())) {
            timekeeper::onNmeaTime(nmea.utc(), nmea.burstStartUs(), NMEA_OFFSET_MS);
        }
    }

    sendGpsInit();

    const bool up = WiFi.status() == WL_CONNECTED;
    if (up && !networkUp) onNetworkUp();
    if (!up && networkUp) Serial.println("WiFi disconnected, reconnecting...");
    networkUp = up;

#if ENABLE_STATUS_PAGE
    if (networkUp) status_page::handle();
#endif

#if STATUS_INTERVAL_S > 0
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint >= STATUS_INTERVAL_S * 1000UL) {
        lastPrint = millis();
        printStatus();
    }
#endif

    delay(1);
}
