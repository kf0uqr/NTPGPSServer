#include "timekeeper.h"
#include "config.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <math.h>

namespace timekeeper {
namespace {

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Written by the PPS interrupt. No floating point in the ISR: the Xtensa
// FPU state is not saved across interrupts.
volatile int64_t lastPpsUs = 0;
volatile int64_t prevPpsUs = 0;
volatile uint32_t ppsCount = 0;

// The anchor: local timer reading `anchorUs` corresponds exactly to the
// start of UTC second `anchorSec`. Protected by `mux`.
bool haveAnchor = false;
int64_t anchorSec = 0;
int64_t anchorUs = 0;
SyncSource source = SyncSource::None;

// Local timer microseconds per true second, measured from the PPS.
double rateUsPerSec = 1e6;
uint32_t rateSamples = 0;

// PPS pulses further than this from 1 s apart are treated as noise or a
// missed pulse and not used for frequency measurement.
const int64_t MAX_INTERVAL_ERR_US = 500;

// If the NMEA sentence arrives this long after the pulse we can no longer be
// sure it describes that pulse rather than the next one.
const int64_t MAX_PPS_TO_NMEA_US = 900000;

// PPS is considered present if a pulse was seen this recently.
const int64_t PPS_PRESENT_US = 3000000;

// Treated as holdover once GPS updates stop for this long.
const int64_t HOLDOVER_AFTER_US = 3000000;

void IRAM_ATTR ppsIsr() {
    const int64_t t = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&mux);
    prevPpsUs = lastPpsUs;
    lastPpsUs = t;
    ppsCount = ppsCount + 1;
    portEXIT_CRITICAL_ISR(&mux);
}

// Convert a local timer reading to UTC. Caller holds `mux` or has copied
// the anchor state.
void convert(int64_t localUs, int64_t aSec, int64_t aUs, double rate,
             int64_t &unixSec, uint32_t &micros) {
    const double elapsedTrueUs = (double)(localUs - aUs) * (1e6 / rate);
    const int64_t total = aSec * 1000000LL + (int64_t)llround(elapsedTrueUs);
    unixSec = total / 1000000LL;
    micros = (uint32_t)(total % 1000000LL);
}

}  // namespace

void begin(int ppsPin, bool risingEdge) {
    pinMode(ppsPin, INPUT);
    attachInterrupt(digitalPinToInterrupt(ppsPin), ppsIsr, risingEdge ? RISING : FALLING);
}

void onNmeaTime(time_t utc, int64_t burstStartUs, int nmeaOffsetMs) {
    const int64_t nowUs = esp_timer_get_time();

    portENTER_CRITICAL(&mux);
    const int64_t pps = lastPpsUs;
    const int64_t prev = prevPpsUs;
    portEXIT_CRITICAL(&mux);

    const int64_t ppsAge = nowUs - pps;
    const bool ppsPresent = pps != 0 && ppsAge < PPS_PRESENT_US;

    if (ppsPresent) {
        // Ambiguous which pulse this sentence belongs to; skip it and keep
        // running on the previous anchor.
        if (ppsAge > MAX_PPS_TO_NMEA_US) return;

        const int64_t interval = pps - prev;
        const bool goodInterval =
            prev != 0 && llabs(interval - 1000000LL) <= MAX_INTERVAL_ERR_US;

        portENTER_CRITICAL(&mux);
        if (goodInterval) {
            // Running average; converges quickly at first, then smooths the
            // few-microsecond interrupt latency jitter.
            rateSamples++;
            const double alpha = rateSamples < 32 ? 1.0 / rateSamples : 1.0 / 32;
            rateUsPerSec += ((double)interval - rateUsPerSec) * alpha;
        }
        anchorSec = utc;
        anchorUs = pps;
        haveAnchor = true;
        source = SyncSource::Pps;
        portEXIT_CRITICAL(&mux);
        return;
    }

    // No PPS: use the start of the NMEA burst as the second marker.
    if (nowUs - burstStartUs > 1000000LL) return;
    portENTER_CRITICAL(&mux);
    anchorSec = utc;
    anchorUs = burstStartUs - (int64_t)nmeaOffsetMs * 1000;
    haveAnchor = true;
    source = SyncSource::Nmea;
    portEXIT_CRITICAL(&mux);
}

bool at(int64_t localUs, int64_t &unixSec, uint32_t &micros) {
    portENTER_CRITICAL(&mux);
    const bool ok = haveAnchor;
    const int64_t aSec = anchorSec, aUs = anchorUs;
    const double rate = rateUsPerSec;
    portEXIT_CRITICAL(&mux);
    if (!ok) return false;
    convert(localUs, aSec, aUs, rate, unixSec, micros);
    return true;
}

bool now(int64_t &unixSec, uint32_t &micros) {
    return at(esp_timer_get_time(), unixSec, micros);
}

bool lastReference(int64_t &unixSec, uint32_t &micros) {
    portENTER_CRITICAL(&mux);
    const bool ok = haveAnchor;
    const int64_t aSec = anchorSec;
    portEXIT_CRITICAL(&mux);
    unixSec = aSec;
    micros = 0;
    return ok;
}

TimeStatus status() {
    const int64_t nowUs = esp_timer_get_time();
    TimeStatus s{};

    portENTER_CRITICAL(&mux);
    const bool ok = haveAnchor;
    const int64_t aUs = anchorUs;
    s.source = source;
    s.ppsCount = ppsCount;
    const double rate = rateUsPerSec;
    const uint32_t samples = rateSamples;
    portEXIT_CRITICAL(&mux);

    s.freqPpm = samples ? rate - 1e6 : 0.0;  // 1 us per s == 1 ppm
    if (!ok) {
        s.source = SyncSource::None;
        return s;
    }

    const int64_t since = nowUs - aUs;
    s.sinceSyncS = (uint32_t)(since / 1000000LL);
    s.holdover = since > HOLDOVER_AFTER_US;
    s.synced = since < (int64_t)HOLDOVER_LIMIT_S * 1000000LL;

    // Base error of the reference, plus crystal drift while in holdover.
    s.dispersionS = (s.source == SyncSource::Pps) ? 20e-6 : 50e-3;
    if (s.holdover) s.dispersionS += (since / 1e6) * HOLDOVER_DRIFT_PPM * 1e-6;
    return s;
}

}  // namespace timekeeper
