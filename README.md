# ESP32 GPS NTP Server

A stratum-1 NTP server built from an ESP32 dev board and a QRP Labs GPS
receiver module (the kind that ships with QMX/QMX+ kits, or a QLG-series board).

The GPS 1PPS pulse is timestamped in an interrupt. The NMEA `RMC` sentence
that follows tells the ESP32 which UTC second the pulse was. The ESP32 then
answers NTP requests on UDP port 123 over WiFi. There is also a small status
page on port 80.

## Hardware

| GPS module pin | ESP32 pin | Notes |
|---|---|---|
| GND | GND | |
| VCC / power in | 3V3 or 5V (VIN) | Use whatever supply voltage your module's manual specifies |
| TX / serial data out | GPIO16 | NMEA, 9600 baud by default |
| RX / serial data in | GPIO17 | Optional. The firmware never sends anything to the GPS |
| 1PPS | GPIO4 | Strongly recommended. Without it you get ~10 ms accuracy instead of microseconds |

You can change the pins in `include/config.h`.

### Check these on the QRP Labs module before you wire it up

QRP Labs has shipped several GPS boards and revisions, so confirm these with
the module's own manual or a multimeter/scope:

1. **Logic levels.** The ESP32 is a **3.3 V part and its pins are not 5 V
   tolerant.** If the module's TX or 1PPS swings to 5 V, add a divider on each
   line. For example, 10 kΩ in series and 20 kΩ to ground gives about 3.3 V.
2. **Use the TTL-level serial output, not the RS-232-level one.** Some QRP Labs
   GPS boards have both. An RS-232-level signal (swings negative, up to ±12 V)
   will damage the ESP32.
3. **Baud rate.** It is almost always 9600. If the status output shows
   `nmea=0`, try 4800 or 38400 in `GPS_BAUD`.
4. **PPS polarity.** Most modules pulse high at the top of the second. If yours
   pulses low, set `PPS_RISING_EDGE 0`.
5. **Antenna.** The module needs a clear view of the sky, or an active antenna
   placed by a window, to get a fix. The status line shows the satellite count.

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
[2026-09-23 12:35:19 UTC] src=PPS synced=1 holdover=0 age=0s sats=9 fixq=1 pps=312 freq=+11.84ppm ntp_reqs=4 nmea=1250 cserr=0
```

- `src=PPS` means the 1PPS is being used. `src=NMEA` means PPS isn't being
  seen, so check the wiring and polarity.
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
| `test/host/` | Parser tests that run on your PC (no ESP32 needed) |

Run the host tests with:

```sh
g++ -std=c++17 -Wall -Wextra -Isrc test/host/test_nmea.cpp src/nmea.cpp -o test_nmea && ./test_nmea
```
