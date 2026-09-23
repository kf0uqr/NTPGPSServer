// Host-side tests for the NMEA parser. No ESP32 needed:
//   g++ -std=c++17 -Wall -Wextra -Isrc test/host/test_nmea.cpp src/nmea.cpp -o /tmp/test_nmea && /tmp/test_nmea
#include "nmea.h"

#include <stdio.h>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
    } while (0)

// Build "$<body>*HH\r\n" with a correct checksum.
static std::string sentence(const std::string &body) {
    unsigned char sum = 0;
    for (char c : body) sum ^= (unsigned char)c;
    char tail[8];
    snprintf(tail, sizeof(tail), "*%02X\r\n", sum);
    return "$" + body + tail;
}

// Feed a string; returns how many times feed() reported a valid RMC.
static int feedAll(NmeaParser &p, const std::string &s, int64_t &t, int64_t stepUs = 1000) {
    int hits = 0;
    for (char c : s) { if (p.feed(c, t)) hits++; t += stepUs; }
    return hits;
}

int main() {
    int64_t t = 10000000;

    {   // Valid GN RMC with fractional .00 seconds.
        NmeaParser p;
        std::string s = sentence("GNRMC,123519.00,A,4807.038,N,01131.000,E,0.0,0.0,230926,,,A");
        CHECK(feedAll(p, s, t) == 1);
        CHECK(p.fixValid());
        CHECK(p.utc() == 1790166919);  // 2026-09-23 12:35:19 UTC
    }
    {   // Unix epoch-adjacent date and year-2000 handling.
        NmeaParser p;
        CHECK(feedAll(p, sentence("GPRMC,000000,A,0,N,0,E,0,0,010100,,"), t) == 1);
        CHECK(p.utc() == 946684800);  // 2000-01-01
        CHECK(feedAll(p, sentence("GPRMC,235959,A,0,N,0,E,0,0,311299,,"), t) == 1);
        CHECK(p.utc() == 946684799);  // 1999-12-31 23:59:59
    }
    {   // Void fix is not reported.
        NmeaParser p;
        CHECK(feedAll(p, sentence("GPRMC,123519,V,,,,,,,230926,,"), t) == 0);
        CHECK(!p.fixValid());
    }
    {   // Bad checksum rejected and counted.
        NmeaParser p;
        std::string s = sentence("GPRMC,123519,A,0,N,0,E,0,0,230926,,");
        s[s.size() - 3] = (s[s.size() - 3] == '0') ? '1' : '0';
        CHECK(feedAll(p, s, t) == 0);
        CHECK(p.checksumErrors() == 1);
    }
    {   // Non-whole-second timestamps (e.g. 5 Hz output) are not used as anchors.
        NmeaParser p;
        CHECK(feedAll(p, sentence("GPRMC,123519.20,A,0,N,0,E,0,0,230926,,"), t) == 0);
    }
    {   // GGA fields.
        NmeaParser p;
        feedAll(p, sentence("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"), t);
        CHECK(p.satellites() == 8);
        CHECK(p.fixQuality() == 1);
        CHECK(p.hdop() > 0.89f && p.hdop() < 0.91f);
    }
    {   // Burst start: first '$' after a quiet gap, not later sentences.
        NmeaParser p;
        int64_t start = t + 500000;  // 0.5 s of silence
        t = start;
        feedAll(p, sentence("GPGGA,123519,0,N,0,E,1,05,1.0,0,M,0,M,,"), t);
        feedAll(p, sentence("GPRMC,123519,A,0,N,0,E,0,0,230926,,"), t);
        CHECK(p.burstStartUs() == start);
    }
    {   // Garbage and over-long lines don't crash or report.
        NmeaParser p;
        std::string junk(300, 'A');
        CHECK(feedAll(p, "$" + junk + "\r\n", t) == 0);
        CHECK(feedAll(p, sentence("GPRMC,123519,A,0,N,0,E,0,0,230926,,"), t) == 1);
    }

    printf(failures ? "%d FAILED\n" : "all NMEA tests passed\n", failures);
    return failures ? 1 : 0;
}
