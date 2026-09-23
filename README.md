# ESP32 GPS NTP Server

A stratum-1 NTP server built from an ESP32 dev board and a QRP Labs GPS
receiver module (the kind that ships with QMX/QMX+ kits, or a QLG-series board).

The GPS 1PPS pulse is timestamped in an interrupt. The NMEA `RMC` sentence
that follows tells the ESP32 which UTC second the pulse was. The ESP32 then
answers NTP requests on UDP port 123 over WiFi. There is also a small status
page on port 80.

## Hardware

Written for the **QRP Labs QLG3** (the GPS module for the QMX+). It uses an
EByte E108 receiver (GK9501 chip) and comes with an active antenna. It should
work with any GPS that outputs 3.3 V NMEA and a 1PPS.

| QLG3 signal | ESP32 pin | Notes |
|---|---|---|
| Power | **3V3** | QLG3 needs a *regulated 3.3 V* supply. Do **not** use 5 V/VIN |
| GND | GND | |
| Serial data out | GPIO16 | NMEA at 9600 baud |
| Serial data in (pin 3) | GPIO17 | Used to send the GPS-only command at boot (see below) |
| 1pps out | GPIO4 | Needed for microsecond accuracy |

The QLG3's outputs are 2.8 V logic, which the ESP32 reads fine directly. No
level shifting is needed.

Check which pad is which on your board's silkscreen or in the QMX+ assembly
manual. The QMX+ only wires serial out and 1pps; the ESP32 also uses serial
in (pin 3). QRP Labs' own E108 firmware-fix procedure wires a 3.3 V
microcontroller TX straight to this pin. A 1 kΩ series resistor in that line
is a cheap extra safeguard.

The ESP32 dev board's 3.3 V regulator easily covers the QLG3 and its active
antenna. Put the antenna by a window or outside; the status output shows the
satellite count.

### The QLG3 date bug (why the ESP32 sends a command to the GPS)

QLG3 modules shipped before about August 2025 may have E108 firmware with a
week-rollover bug. When GPS and BeiDou satellites are used together, it
sometimes reports a date about **19.6 years in the past** and the 1pps
becomes unstable. For a time server that would be a disaster. The firmware
defends against it in two ways:

1. **At boot it sends `$PGKC115,1,0,0,0*2B`**, which switches the receiver to
   GPS-only (GLONASS, BeiDou and Galileo off). This avoids the bug, and GPS
   alone is plenty for timing. It needs the GPIO17 → QLG3 serial-in wire. If
   your module has the fixed E108 firmware, you can set `GPS_INIT_COMMANDS` to
   `""` in `include/config.h` to use every constellation.
2. **It sanity-checks everything the GPS says:**
   - Dates before `MIN_VALID_UNIX_TIME` (2026-01-01) are ignored.
   - A time jump is only accepted once 3 fixes in a row agree.
   - A 1pps pulse that doesn't arrive about 1 s after the previous one is
     ignored.

   The counters `bad_dates`, `bad_steps` and `bad_pulses` in the status output
   show whether any of this is happening.

## Building and flashing

This uses [PlatformIO](https://platformio.org/) (CLI or the VS Code extension).

```sh
cp include/secrets.example.h include/secrets.h   # then edit your WiFi SSID/password
pio run -t upload
pio device monitor
```

On the serial monitor you should see something like:

```
WiFi connected, IP 192.168.1.50
NTP server listening on UDP 123
Sent GPS init: $PGKC115,1,0,0,0*2B
[2026-09-23 12:35:19 UTC] src=PPS synced=1 holdover=0 age=0s sats=9 fixq=1 pps=312 freq=+11.84ppm ntp_reqs=4 nmea=1250 cserr=0 bad_dates=0 bad_steps=0 bad_pulses=0
```

- `src=PPS` means the 1PPS is being used. `src=NMEA` means PPS isn't being
  seen, so check the wiring and polarity.
- `nmea=0` means no data is arriving from the GPS. Check the serial-out wire
  and the 3.3 V supply.
- Rising `bad_dates` means your QLG3 has the date bug and the GPS-only command
  isn't reaching it. Check the GPIO17 wire.
- `freq` is the measured error of the ESP32's crystal. It settles after a
  minute or so and is corrected for automatically.
- Browse to `http://<ip>/` (or `http://gps-ntp.local/`) for a live status page.
  `/status.json` returns the same data as JSON.

## Testing it

From another machine on the network:

```sh
# Linux/macOS
ntpdate -q 192.168.1.50            # or: sntp 192.168.1.50
# chrony: add "server 192.168.1.50 iburst" and run: chronyc sources -v
# Windows
w32tm /stripchart /computer:192.168.1.50 /samples:5
```

## How accurate is it?

- **Server clock (with PPS):** within a few microseconds of GPS time. The limit
  is interrupt latency, plus the interpolation between pulses using the
  measured crystal frequency.
- **What clients see over WiFi:** usually 0.5 to 5 ms offset jitter. WiFi, not
  the GPS, is the bottleneck. The firmware turns off WiFi modem sleep, which
  otherwise adds up to hundreds of ms. Clients like chrony filter out much of
  the jitter.
- **Without PPS (NMEA only):** about 10 to 50 ms. Tune `NMEA_OFFSET_MS` against
  a known-good server.

To get much better client-side accuracy, use an ESP32 board with wired
Ethernet (WT32-ETH01, Olimex ESP32-POE, etc.). The timekeeping code doesn't
change; only the network bring-up in `main.cpp` does.

## Behaviour when GPS is lost

The server keeps answering from the free-running ESP32 clock ("holdover"). It
corrects for the last measured frequency error and increases the advertised
root dispersion over time, so clients know to trust it less. After
`HOLDOVER_LIMIT_S` (default 1 hour) it reports leap indicator 3 / stratum 16
(unsynchronised), and clients will stop using it. Until the first GPS fix
after boot it doesn't answer at all.

## Code layout

| File | Purpose |
|---|---|
| `src/main.cpp` | Setup, WiFi, main loop, serial status |
| `src/nmea.*` | Small NMEA parser (RMC and GGA from any talker: GP/GN/GL/...) |
| `src/timekeeper.*` | PPS interrupt, UTC anchor, crystal frequency measurement, holdover |
| `src/ntp_server.*` | NTPv3/v4 server-mode responder on UDP 123 |
| `src/status_page.*` | HTTP status page |
| `include/config.h` | Pins, baud rate and tuning knobs |
| `test/host/` | Parser and timekeeper tests that run on your PC (no ESP32 needed) |

Run the host tests with `test/host/run.sh` (needs only `g++`).
