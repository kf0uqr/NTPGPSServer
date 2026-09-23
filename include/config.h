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
// GPS wiring (defaults are for a QRP Labs QLG3; see README.md)
// ---------------------------------------------------------------------------
// QLG3 power         <- ESP32 3V3 (regulated 3.3 V, NOT 5 V)
// QLG3 serial out    -> ESP32 GPIO16 (UART2 RX)
// QLG3 serial in     <- ESP32 GPIO17 (UART2 TX), needed for GPS_INIT_COMMANDS
// QLG3 1pps          -> ESP32 GPIO4
// QLG3 GND           -- ESP32 GND
#define GPS_RX_PIN   16
#define GPS_TX_PIN   17
#define GPS_BAUD     9600
#define PPS_PIN      4

// 1 = the pulse's rising edge marks the top of the second (almost all modules).
// 0 = falling edge.
#define PPS_RISING_EDGE 1

// Configuration sentences sent to the GPS at boot (via GPIO17), separated by
// "\r\n". Set to "" to send nothing.
//
// The QLG3's EByte E108 module (GK9501 chip) has a firmware bug in units
// shipped before about Aug 2025: when GPS and BeiDou satellites are both
// used, it sometimes reports a date ~19.6 years in the past and its 1pps
// becomes unstable. Restricting it to GPS only avoids the bug, and GPS alone
// is plenty for timing. If your module has the fixed E108 firmware you can
// set this to "" to use all constellations.
//   $PGKC115,<GPS>,<GLONASS>,<BeiDou>,<Galileo>
#define GPS_INIT_COMMANDS "$PGKC115,1,0,0,0*2B\r\n"

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

// GPS times before this are rejected as bogus (2026-01-01 00:00:00 UTC).
// Catches receivers that report a date one GPS week rollover (19.6 years) ago.
#define MIN_VALID_UNIX_TIME 1767225600LL

// Status messages on the USB serial port every N seconds (0 = off).
#define STATUS_INTERVAL_S 10
