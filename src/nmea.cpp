#include "nmea.h"
#include <stdlib.h>
#include <string.h>

// A gap this long between characters means the GPS has finished its burst of
// sentences for one second; the next '$' starts a new burst.
static const int64_t BURST_GAP_US = 200000;

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int twoDigits(const char *p) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm).
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

bool NmeaParser::feed(char c, int64_t now_us) {
    const int64_t gap = now_us - lastCharUs_;
    lastCharUs_ = now_us;

    if (c == '$') {
        if (gap > BURST_GAP_US) burstStartUs_ = now_us;
        inSentence_ = true;
        len_ = 0;
        return false;
    }
    if (!inSentence_) return false;

    if (c == '\r' || c == '\n') {
        inSentence_ = false;
        buf_[len_] = '\0';
        return parseSentence();
    }
    if (len_ >= MAX_LEN) {  // overlong / garbage
        inSentence_ = false;
        return false;
    }
    buf_[len_++] = c;
    return false;
}

bool NmeaParser::parseSentence() {
    // Verify "*HH" checksum (XOR of everything between '$' and '*').
    char *star = strrchr(buf_, '*');
    if (!star || star + 3 > buf_ + len_) { csErrors_++; return false; }
    int hi = hexVal(star[1]), lo = hexVal(star[2]);
    uint8_t sum = 0;
    for (char *p = buf_; p < star; p++) sum ^= (uint8_t)*p;
    if (hi < 0 || lo < 0 || sum != (uint8_t)((hi << 4) | lo)) { csErrors_++; return false; }
    *star = '\0';
    sentences_++;

    // Split on commas in place. Empty fields become empty strings.
    char *f[24];
    int n = 0;
    f[n++] = buf_;
    for (char *p = buf_; *p && n < 24; p++) {
        if (*p == ',') { *p = '\0'; f[n++] = p + 1; }
    }
    if (strlen(f[0]) != 5) return false;
    const char *type = f[0] + 2;  // skip talker ID

    if (strcmp(type, "RMC") == 0) return handleRmc(f, n);
    if (strcmp(type, "GGA") == 0) handleGga(f, n);
    return false;
}

bool NmeaParser::handleRmc(char **f, int n) {
    // $xxRMC,hhmmss.ss,A,llll.ll,a,yyyyy.yy,a,x.x,x.x,ddmmyy,...
    if (n < 10) return false;
    fixValid_ = (f[2][0] == 'A');
    const char *t = f[1], *d = f[9];
    if (!fixValid_ || strlen(t) < 6 || strlen(d) != 6) return false;

    int hh = twoDigits(t), mm = twoDigits(t + 2), ss = twoDigits(t + 4);
    int day = twoDigits(d), mon = twoDigits(d + 2), yy = twoDigits(d + 4);
    if (hh < 0 || mm < 0 || ss < 0 || day < 1 || mon < 1 || mon > 12 || yy < 0) return false;
    if (hh > 23 || mm > 59 || ss > 60) return false;

    // Two-digit year: 80-99 -> 19xx, 00-79 -> 20xx.
    int year = yy + (yy >= 80 ? 1900 : 2000);
    utc_ = (time_t)(daysFromCivil(year, mon, day) * 86400LL + hh * 3600 + mm * 60 + ss);

    // Fractional seconds in the timestamp mean the module is reporting at a
    // rate other than 1 Hz on the second; ignore those for anchoring.
    const char *dot = strchr(t, '.');
    if (dot) {
        for (const char *p = dot + 1; *p; p++)
            if (*p != '0') return false;
    }
    return true;
}

void NmeaParser::handleGga(char **f, int n) {
    // $xxGGA,hhmmss.ss,llll.ll,a,yyyyy.yy,a,q,ss,h.h,...
    if (n < 9) return;
    quality_ = (uint8_t)atoi(f[6]);
    sats_ = (uint8_t)atoi(f[7]);
    hdop_ = (float)atof(f[8]);
}
