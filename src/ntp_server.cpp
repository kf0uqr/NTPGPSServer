#include "ntp_server.h"
#include "timekeeper.h"

#include <AsyncUDP.h>
#include <esp_timer.h>
#include <string.h>

namespace ntp_server {
namespace {

AsyncUDP udp;
bool listening = false;
volatile uint32_t served = 0;

const uint16_t NTP_PORT = 123;
const int NTP_PACKET_LEN = 48;
const int64_t NTP_UNIX_OFFSET = 2208988800LL;  // 1900-01-01 -> 1970-01-01

// log2 of the clock read resolution: esp_timer ticks in 1 us (~2^-20 s).
const int8_t PRECISION = -20;

void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

// NTP 64-bit timestamp: 32-bit seconds since 1900 (wraps in 2036, which
// the protocol handles by era) + 32-bit binary fraction.
void putTimestamp(uint8_t *p, int64_t unixSec, uint32_t micros) {
    put32(p, (uint32_t)(unixSec + NTP_UNIX_OFFSET));
    put32(p + 4, (uint32_t)(((uint64_t)micros << 32) / 1000000ULL));
}

// NTP 32-bit short format: 16.16 fixed-point seconds.
uint32_t toShort(double seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds > 65535) seconds = 65535;
    return (uint32_t)(seconds * 65536.0);
}

void handlePacket(AsyncUDPPacket &packet) {
    // Timestamp arrival as early as we can.
    const int64_t rxLocalUs = esp_timer_get_time();

    if (packet.length() < NTP_PACKET_LEN) return;
    const uint8_t *req = packet.data();
    const uint8_t mode = req[0] & 0x07;
    if (mode != 3) return;  // only answer client requests
    uint8_t version = (req[0] >> 3) & 0x07;
    if (version < 1 || version > 4) return;

    int64_t rxSec;
    uint32_t rxUs;
    if (!timekeeper::at(rxLocalUs, rxSec, rxUs)) return;  // never synced: stay silent

    const TimeStatus st = timekeeper::status();
    const uint8_t leap = st.synced ? 0 : 3;  // 3 = alarm, clock not synchronised
    const uint8_t stratum = st.synced ? 1 : 16;

    uint8_t resp[NTP_PACKET_LEN];
    memset(resp, 0, sizeof(resp));
    resp[0] = (leap << 6) | (version << 3) | 4;  // mode 4 = server
    resp[1] = stratum;
    resp[2] = req[2];  // echo client's poll interval
    resp[3] = (uint8_t)PRECISION;
    put32(resp + 4, 0);  // root delay: we are the reference
    put32(resp + 8, toShort(st.dispersionS));
    memcpy(resp + 12, st.source == SyncSource::Pps ? "PPS" : "GPS", 4);

    int64_t refSec;
    uint32_t refUs;
    timekeeper::lastReference(refSec, refUs);
    putTimestamp(resp + 16, refSec, refUs);
    memcpy(resp + 24, req + 40, 8);  // origin = client's transmit timestamp
    putTimestamp(resp + 32, rxSec, rxUs);

    int64_t txSec;
    uint32_t txUs;
    timekeeper::now(txSec, txUs);
    putTimestamp(resp + 40, txSec, txUs);

    packet.write(resp, sizeof(resp));
    served = served + 1;
}

}  // namespace

bool begin() {
    if (listening) return true;
    if (!udp.listen(NTP_PORT)) return false;
    udp.onPacket(handlePacket);
    listening = true;
    return true;
}

uint32_t requestsServed() { return served; }

}  // namespace ntp_server
