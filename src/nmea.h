#pragma once
#include <stdint.h>
#include <time.h>

// Minimal NMEA 0183 parser. Only understands RMC (time/date/validity) and
// GGA (fix quality, satellites, HDOP), from any talker (GP, GN, GL, BD...).
class NmeaParser {
public:
    // Feed one character received from the GPS. `now_us` is the local
    // microsecond clock at the time the character was read.
    // Returns true when a complete, checksum-valid RMC sentence with a valid
    // fix has just been parsed; the time it carries is then in utc().
    bool feed(char c, int64_t now_us);

    time_t   utc() const        { return utc_; }
    int64_t  burstStartUs() const { return burstStartUs_; }
    bool     fixValid() const   { return fixValid_; }
    uint8_t  satellites() const { return sats_; }
    uint8_t  fixQuality() const { return quality_; }
    float    hdop() const       { return hdop_; }
    uint32_t sentences() const  { return sentences_; }
    uint32_t checksumErrors() const { return csErrors_; }

private:
    bool parseSentence();
    bool handleRmc(char **f, int n);
    void handleGga(char **f, int n);

    static const int MAX_LEN = 100;
    char buf_[MAX_LEN + 1];
    int len_ = 0;
    bool inSentence_ = false;

    int64_t lastCharUs_ = 0;
    int64_t burstStartUs_ = 0;

    time_t utc_ = 0;
    bool fixValid_ = false;
    uint8_t sats_ = 0;
    uint8_t quality_ = 0;
    float hdop_ = 0;
    uint32_t sentences_ = 0;
    uint32_t csErrors_ = 0;
};
