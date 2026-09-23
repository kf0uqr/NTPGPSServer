#pragma once
#include <stdint.h>
#include <time.h>

// Keeps UTC by anchoring the ESP32's 64-bit microsecond timer to the GPS.
//
// With PPS: the pulse edge is timestamped in an interrupt, and the NMEA RMC
// sentence that follows says which UTC second that edge was. The interval
// between successive pulses is used to measure (and correct for) the ESP32
// crystal's frequency error when interpolating between pulses.
//
// Without PPS: the start of each NMEA burst is used as a rough (tens of ms)
// marker of the second.

enum class SyncSource : uint8_t { None, Nmea, Pps };

struct TimeStatus {
    SyncSource source;
    bool synced;          // time is good enough to serve
    bool holdover;        // lost GPS, free-running on the local clock
    uint32_t sinceSyncS;  // seconds since the last GPS update
    double freqPpm;       // measured ESP32 clock error (+ = local clock fast)
    uint32_t ppsCount;
    double dispersionS;   // estimated error bound, for NTP root dispersion
    uint32_t rejectedDates;  // GPS times before MIN_VALID_UNIX_TIME
    uint32_t rejectedSteps;  // time jumps ignored as probable GPS glitches
    uint32_t acceptedSteps;  // time jumps accepted after confirmation
    uint32_t badPulses;      // PPS pulses not ~1 s after the previous one
};

namespace timekeeper {

void begin(int ppsPin, bool risingEdge);

// Called from the main loop whenever the NMEA parser reports a valid RMC.
// `utc` is the second the sentence describes; `burstStartUs` is when the
// burst of sentences it belongs to started arriving.
void onNmeaTime(time_t utc, int64_t burstStartUs, int nmeaOffsetMs);

// Current UTC as seconds + microseconds since the Unix epoch.
// Safe to call from any task. Returns false if we have never synced.
bool now(int64_t &unixSec, uint32_t &micros);

// Same, for an arbitrary esp_timer_get_time() reading taken earlier.
bool at(int64_t localUs, int64_t &unixSec, uint32_t &micros);

// UTC of the last reference update (for the NTP reference timestamp).
bool lastReference(int64_t &unixSec, uint32_t &micros);

TimeStatus status();

}  // namespace timekeeper
