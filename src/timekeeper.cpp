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

// Consecutive fixes that must agree before accepting a time step.
const uint32_t STEP_CONFIRMATIONS = 3;

// Sanity-check state and counters. Only touched from the main loop.
int64_t pendingStep = 0;
uint32_t pendingStepCount = 0;
uint32_t rejectedDates = 0;
uint32_t rejectedSteps = 0;
uint32_t acceptedSteps = 0;
uint32_t badPulses = 0;

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

// Decide whether a new (utc, localUs) anchor is believable, given the
// current one. GPS receivers occasionally report a wrong date or time (the
// QLG3's E108 module is known to jump back ~19.6 years), so a jump of a
// second or more is only accepted once several consecutive fixes agree.
bool acceptAnchor(time_t utc, int64_t localUs) {
    if (utc < (time_t)MIN_VALID_UNIX_TIME) {
        rejectedDates++;
        return false;
    }

    portENTER_CRITICAL(&mux);
    const bool anchored = haveAnchor;
    const int64_t aSec = anchorSec, aUs = anchorUs;
    const double rate = rateUsPerSec;
    portEXIT_CRITICAL(&mux);
    if (!anchored) return true;

    int64_t predSec;
    uint32_t predUs;
    convert(localUs, aSec, aUs, rate, predSec, predUs);
    if (predUs >= 500000) predSec++;  // round to the nearest second
    const int64_t offset = (int64_t)utc - predSec;

    if (offset == 0) {
        pendingStepCount = 0;
        return true;
    }
    if (offset == pendingStep) {
        pendingStepCount++;
    } else {
        pendingStep = offset;
        pendingStepCount = 1;
    }
    if (pendingStepCount < STEP_CONFIRMATIONS) {
        rejectedSteps++;
        return false;
    }
    pendingStepCount = 0;
    acceptedSteps++;
    return true;
}

void setAnchor(time_t utc, int64_t localUs, SyncSource src) {
    portENTER_CRITICAL(&mux);
    anchorSec = utc;
    anchorUs = localUs;
    haveAnchor = true;
    source = src;
    portEXIT_CRITICAL(&mux);
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
    const bool wasPps = haveAnchor && source == SyncSource::Pps &&
                        nowUs - anchorUs < PPS_PRESENT_US;
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

        // While locked, a pulse that isn't ~1 s after the previous one is a
        // glitch or follows a missed pulse. Don't move the anchor to it.
        if (wasPps && !goodInterval) {
            badPulses++;
            return;
        }
        if (!acceptAnchor(utc, pps)) return;

        if (goodInterval) {
            // Running average; converges quickly at first, then smooths the
            // few-microsecond interrupt latency jitter.
            portENTER_CRITICAL(&mux);
            rateSamples++;
            const double alpha = rateSamples < 32 ? 1.0 / rateSamples : 1.0 / 32;
            rateUsPerSec += ((double)interval - rateUsPerSec) * alpha;
            portEXIT_CRITICAL(&mux);
        }
        setAnchor(utc, pps, SyncSource::Pps);
        return;
    }

    // No PPS: use the start of the NMEA burst as the second marker.
    if (nowUs - burstStartUs > 1000000LL) return;
    const int64_t markUs = burstStartUs - (int64_t)nmeaOffsetMs * 1000;
    if (!acceptAnchor(utc, markUs)) return;
    setAnchor(utc, markUs, SyncSource::Nmea);
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
    s.rejectedDates = rejectedDates;
    s.rejectedSteps = rejectedSteps;
    s.acceptedSteps = acceptedSteps;
    s.badPulses = badPulses;
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
