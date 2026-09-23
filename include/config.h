#pragma once

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------
// WiFi credentials live in include/secrets.h (git-ignored). Copy
// include/secrets.example.h to include/secrets.h and fill it in.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing include/secrets.h - copy include/secrets.example.h to include/secrets.h and set your WiFi credentials"
#endif

#define HOSTNAME "gps-ntp"

// Serve a small status page on http://<ip>/ (and JSON on /status.json).
#define ENABLE_STATUS_PAGE 1

// ---------------------------------------------------------------------------
// GPS wiring
// ---------------------------------------------------------------------------
// GPS TX (NMEA out)  -> ESP32 GPIO16 (UART2 RX)
// GPS RX (optional)  -> ESP32 GPIO17 (UART2 TX)
// GPS 1PPS           -> ESP32 GPIO4
// All signals must be 3.3 V logic. Do NOT connect an RS-232 level output.
#define GPS_RX_PIN   16
#define GPS_TX_PIN   17
#define GPS_BAUD     9600
#define PPS_PIN      4

// 1 = the pulse's rising edge marks the top of the second (almost all modules).
// 0 = falling edge.
#define PPS_RISING_EDGE 1

// ---------------------------------------------------------------------------
// Timekeeping
// ---------------------------------------------------------------------------
// When no PPS is available the server falls back to NMEA sentence timing.
// This is the typical delay (ms) between the true top of the second and the
// start of the NMEA burst. Tune it by comparing against a known-good NTP
// server; it only matters when PPS is not wired up.
#define NMEA_OFFSET_MS 80

// How long (seconds) to keep serving time from the free-running ESP32 clock
// after losing GPS before telling clients we are unsynchronised.
#define HOLDOVER_LIMIT_S 3600

// Assumed worst-case drift of the ESP32 crystal during holdover, in ppm.
// Used to grow the advertised root dispersion.
#define HOLDOVER_DRIFT_PPM 20.0

// Status messages on the USB serial port every N seconds (0 = off).
#define STATUS_INTERVAL_S 10
