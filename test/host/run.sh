#!/bin/sh
# Build and run the host-side tests (needs only g++).
set -e
cd "$(dirname "$0")/../.."
OUT="${TMPDIR:-/tmp}/ntpgps-tests"
mkdir -p "$OUT"
g++ -std=gnu++17 -Wall -Wextra -Isrc test/host/test_nmea.cpp src/nmea.cpp -o "$OUT/test_nmea"
g++ -std=gnu++17 -Wall -Wextra -Itest/host/stubs -Iinclude -Isrc \
    test/host/test_timekeeper.cpp src/timekeeper.cpp -o "$OUT/test_timekeeper"
"$OUT/test_nmea"
"$OUT/test_timekeeper"
