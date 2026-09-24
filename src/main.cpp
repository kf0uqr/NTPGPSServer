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

// GPS link diagnostics.
static uint32_t gpsRxBytes = 0;
static const uint32_t BAUD_RATES[] = {GPS_BAUD, 4800, 9600, 38400, 115200};
static size_t baudIndex = 0;
static uint32_t baudTriedAtMs = 0;
static uint32_t baudTriedAtBytes = 0;
static int echoedLines = 0;

const char *sourceName(SyncSource s) {
    switch (s) {
        case SyncSource::Pps:  return "PPS";
        case SyncSource::Nmea: return "NMEA";
        default:               return "none";
    }
}

static const char *wifiStateName(wl_status_t s) {
    switch (s) {
        case WL_CONNECTED:      return "connected";
        case WL_NO_SSID_AVAIL:  return "ssid-not-found";
        case WL_CONNECT_FAILED: return "connect-failed";
        case WL_CONNECTION_LOST: return "connection-lost";
        case WL_DISCONNECTED:   return "disconnected";
        case WL_IDLE_STATUS:    return "idle";
        default:                return "connecting";
    }
}

// Echo the first few raw lines from the GPS so wiring and baud problems are
// visible: good data looks like "$GNRMC,...", a wrong baud rate looks like
// garbage.
static void echoGpsLine(char c) {
    static const int MAX_LINES = 5;
    static char buf[90];
    static size_t len = 0;
    if (echoedLines >= MAX_LINES) return;
    if (c == '\n' || len >= sizeof(buf) - 5) {
        buf[len] = '\0';
        Serial.printf("GPS raw: %s\n", buf);
        len = 0;
        echoedLines++;
        return;
    }
    if (c == '\r') return;
    if (c >= 0x20 && c < 0x7f) {
        buf[len++] = c;
    } else {
        len += snprintf(buf + len, sizeof(buf) - len, "<%02X>", (uint8_t)c);
    }
}

// If bytes are arriving but none of them form valid NMEA sentences, the baud
// rate is probably wrong: step through the common ones.
static void huntBaudRate() {
    if (nmea.sentences() > 0) return;
    const uint32_t now = millis();
    if (now - baudTriedAtMs < 4000) return;
    const bool gotBytes = gpsRxBytes - baudTriedAtBytes > 100;
    baudTriedAtMs = now;
    baudTriedAtBytes = gpsRxBytes;
    if (!gotBytes) return;  // silence: baud rate isn't the problem
    const uint32_t from = BAUD_RATES[baudIndex];
    baudIndex = (baudIndex + 1) % (sizeof(BAUD_RATES) / sizeof(BAUD_RATES[0]));
    if (BAUD_RATES[baudIndex] == from)
        baudIndex = (baudIndex + 1) % (sizeof(BAUD_RATES) / sizeof(BAUD_RATES[0]));
    Serial.printf("GPS: data arriving but no valid NMEA at %u baud, trying %u\n",
                  (unsigned)from, (unsigned)BAUD_RATES[baudIndex]);
    gpsSerial.updateBaudRate(BAUD_RATES[baudIndex]);
    echoedLines = 0;  // show what the new baud rate looks like
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
        "bad_dates=%u bad_steps=%u bad_pulses=%u gps_rx_bytes=%u baud=%u wifi=%s\n",
        when, sourceName(st.source), st.synced, st.holdover, (unsigned)st.sinceSyncS,
        nmea.satellites(), nmea.fixQuality(), (unsigned)st.ppsCount, st.freqPpm,
        (unsigned)ntp_server::requestsServed(), (unsigned)nmea.sentences(),
        (unsigned)nmea.checksumErrors(), (unsigned)st.rejectedDates,
        (unsigned)st.rejectedSteps, (unsigned)st.badPulses, (unsigned)gpsRxBytes,
        (unsigned)BAUD_RATES[baudIndex], wifiStateName(WiFi.status()));

    // A GPS sends several hundred bytes a second, fix or no fix. A stray
    // byte or two is just noise from power-up.
    if (gpsRxBytes < 50 && millis() > 5000) {
        Serial.printf(
            "  !! No data from the GPS on GPIO%d. Check: GPS serial-out -> GPIO%d "
            "(labelled RX2 on many boards), GND shared, GPS powered from 3V3. "
            "GPIO16/17 don't work on WROVER/PSRAM boards.\n",
            GPS_RX_PIN, GPS_RX_PIN);
    }
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
        gpsRxBytes++;
        echoGpsLine(c);
        if (nmea.feed(c, esp_timer_get_time())) {
            timekeeper::onNmeaTime(nmea.utc(), nmea.burstStartUs(), NMEA_OFFSET_MS);
        }
    }

    sendGpsInit();
    huntBaudRate();

    const bool up = WiFi.status() == WL_CONNECTED;
    if (up && !networkUp) onNetworkUp();
    if (!up && networkUp) Serial.println("WiFi disconnected, reconnecting...");
    networkUp = up;

    static wl_status_t lastWifi = WL_IDLE_STATUS;
    const wl_status_t wifi = WiFi.status();
    if (wifi != lastWifi && wifi != WL_CONNECTED) {
        Serial.printf("WiFi: %s (SSID \"%s\")\n", wifiStateName(wifi), WIFI_SSID);
    }
    lastWifi = wifi;

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
