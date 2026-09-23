// Host-side tests for the timekeeper, driven by a fake microsecond clock and
// a fake PPS interrupt. Run with test/host/run.sh.
#include "config.h"
#include "timekeeper.h"

#include <stdio.h>

static int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
    } while (0)

static int64_t fakeNowUs = 0;
static void (*ppsIsr)() = nullptr;
int64_t esp_timer_get_time() { return fakeNowUs; }
void attachInterrupt(int, void (*isr)(), int) { ppsIsr = isr; }

static const time_t T0 = 1790166919;  // 2026-09-23 12:35:19 UTC
static const int64_t NMEA_DELAY_US = 300000;

// One GPS second: PPS edge at local time `ppsUs`, RMC saying `utc` shortly after.
static void second(int64_t ppsUs, time_t utc, bool pulse = true) {
    if (pulse) { fakeNowUs = ppsUs; ppsIsr(); }
    fakeNowUs = ppsUs + NMEA_DELAY_US;
    timekeeper::onNmeaTime(utc, fakeNowUs - 50000, 80);
}

int main() {
    timekeeper::begin(4, true);
    CHECK(ppsIsr != nullptr);

    int64_t sec;
    uint32_t us;
    CHECK(!timekeeper::now(sec, us));  // nothing until the first fix

    // Local crystal running 10 ppm fast: 1,000,010 local us per real second.
    const int64_t RATE = 1000010, BASE = 5000000;
    int n = 0;
    for (; n < 60; n++) second(BASE + n * RATE, T0 + n);

    TimeStatus st = timekeeper::status();
    CHECK(st.source == SyncSource::Pps);
    CHECK(st.synced && !st.holdover);
    CHECK(st.freqPpm > 9.9 && st.freqPpm < 10.1);

    // Half a real second after the last pulse.
    CHECK(timekeeper::at(BASE + (n - 1) * RATE + RATE / 2, sec, us));
    CHECK(sec == T0 + n - 1);
    CHECK(us > 499990 && us < 500010);

    // The E108 bug: a date ~19.6 years ago must be rejected outright.
    second(BASE + n * RATE, T0 + n - 1024LL * 7 * 86400);
    CHECK(timekeeper::status().rejectedDates == 1);
    CHECK(timekeeper::at(BASE + n * RATE, sec, us) && sec == T0 + n);
    n++;

    // A one-off wrong second (plausible date) is ignored...
    second(BASE + n * RATE, T0 + n + 5);
    CHECK(timekeeper::status().rejectedSteps == 1);
    CHECK(timekeeper::at(BASE + n * RATE, sec, us) && sec == T0 + n);
    n++;
    for (int i = 0; i < 3; i++, n++) second(BASE + n * RATE, T0 + n);
    CHECK(timekeeper::status().acceptedSteps == 0);

    // ...but a real, persistent step is accepted after 3 agreeing fixes.
    for (int i = 0; i < 3; i++, n++) second(BASE + n * RATE, T0 + n + 1);
    CHECK(timekeeper::status().acceptedSteps == 1);
    CHECK(timekeeper::at(BASE + (n - 1) * RATE, sec, us) && sec == T0 + n);

    // A glitch pulse mid-second doesn't move the anchor.
    fakeNowUs = BASE + n * RATE - 400000;
    ppsIsr();
    fakeNowUs += 10000;
    timekeeper::onNmeaTime(T0 + n, fakeNowUs, 80);
    CHECK(timekeeper::status().badPulses == 1);
    CHECK(timekeeper::at(BASE + n * RATE, sec, us) && sec == T0 + n + 1);
    CHECK(us < 10 || us > 999990);

    // The next real pulse is only 0.4 s after the glitch, so it's skipped
    // too; after that the server is back to normal.
    for (int i = 0; i < 3; i++, n++) second(BASE + n * RATE, T0 + n + 1);
    st = timekeeper::status();
    CHECK(st.badPulses == 2);
    CHECK(st.synced && st.source == SyncSource::Pps);
    CHECK(timekeeper::at(BASE + (n - 1) * RATE + 250000, sec, us) && sec == T0 + n);

    // GPS lost: holdover, then unsynchronised after HOLDOVER_LIMIT_S.
    fakeNowUs = BASE + (n - 1) * RATE + 10LL * RATE;
    st = timekeeper::status();
    CHECK(st.synced && st.holdover);
    fakeNowUs = BASE + (n - 1) * RATE + (HOLDOVER_LIMIT_S + 1) * RATE;
    CHECK(!timekeeper::status().synced);

    printf(failures ? "%d FAILED\n" : "all timekeeper tests passed\n", failures);
    return failures ? 1 : 0;
}
