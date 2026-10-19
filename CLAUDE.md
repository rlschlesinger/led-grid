# LED Grid: notes for Claude Code on a new machine

The firmware runs a 16×16 WS2812B panel (or two chained into 32×16) on a classic ESP32 DevKit.
Use this file to build it and flash it onto your own boards. README.md covers wiring and the
web page. Hardware is the user's own, so confirm before flashing a board that may be running
something else.

## One-time setup

1. Install PlatformIO Core (`brew install platformio` on macOS, or `pipx install platformio`).
   The first build downloads the ESP32 toolchain, FastLED 3.7.8 and ArduinoJson 7.2.1.
2. Create `ota.ini` at the repo root. It is gitignored, and the build fails without it
   (`No section: 'ota'`):

   ```bash
   printf '[ota]\npassword = %s\n' "$(openssl rand -hex 16)" > ota.ini
   ```

   This password is baked into the firmware for wireless updates. Keep the file: later Wi-Fi
   flashes of the same boards need the same password.
3. Check that it builds: `pio run` (one panel) or `pio run -e wide` (two panels).

## Flash over USB (first time, per board)

| Board's USB chip | Env | Port it shows up as (macOS) |
|---|---|---|
| CP2102 | `esp32dev` (default) | `/dev/cu.usbserial-*` |
| CH340 | `wide` (32×16), or `esp32dev` with `--upload-port` | `/dev/cu.wchusbserial*` |

```bash
pio device list                                   # find the board
pio run -t upload                                 # 16×16, CP2102
pio run -e wide -t upload                         # 32×16, CH340
pio run -t upload --upload-port /dev/cu.XXXX      # any other combination
```

- Ports are pinned in `platformio.ini`, because auto-detect grabs Bluetooth headphones or USB-C
  dock devices. On Linux/Windows the patterns won't match: pass `--upload-port`
  (`/dev/ttyUSB0`, `COM3`, …).
- If 921600 baud upload fails, add `--upload-speed 115200` or try another cable. Some boards
  also need BOOT held while "Connecting…" appears.
- The DIN data wire goes on GPIO 13 (`DATA_PIN` in `src/main.cpp`).

## After flashing

1. The board starts its own network, **LED-Grid** (password `ledgrid123`), at
   http://192.168.4.1. Phones open the page by themselves (captive portal).
2. Select **Layout test** and toggle **Setup → Panel layout** (zigzag, columns, flips) until it
   matches the on-page description. These settings are stored on the board, so no rebuild is needed.
3. Set **Power limit** to just under the 5 V supply's rating. The default 500 mA is safe on USB.
4. Home Wi-Fi: use **Setup → Wi-Fi** on the page, or over USB run
   `python3 tools/set-wifi.py "Network"`. It needs pyserial; PlatformIO's own Python has it,
   which `head -1 $(which pio)` shows. The board restarts, joins, and scrolls its IP.
5. Set the location (weather, sun/moon, flights) and the teams to follow on the page.
   Coordinates are rounded to ~1 km on purpose. Don't commit anyone's home address.

## Flash over Wi-Fi (after the board is on your network)

```bash
pio run -e ota -t upload --upload-port <board IP>         # 16×16
pio run -e wide-ota -t upload --upload-port <board IP>    # 32×16
```

Always pass `--upload-port`. The `ota` env defaults to 192.168.1.240, which is the original
owner's grid. On another network that address could be any device. OTA needs the same
`ota.ini` password the board was flashed with. A board flashed with a different password has
to be reflashed over USB once.

## Debugging

- Serial log: `pio device monitor` (115200 baud). It prints the AP and home-network addresses.
  Opening the port resets the board.
- `GET /api/state` returns settings, uptime and the last reset reason. Reason 6 is the task
  watchdog.
- `pio run -e diag -t upload` flashes a bare wiring test (`src/diag/diag.cpp`).
- Garbage or nothing on the panel usually means a missing common ground, data on the DOUT end,
  or (less often) a 3.3 V signal level that needs a 74AHCT125 level shifter.

## Code map

- `src/main.cpp`: hardware constants, effects, settings (NVS), web server/API, Wi-Fi, OTA.
- `src/ambient.h`: quarter-hour card (time, Open-Meteo weather, sun path, moon phase).
- `src/games.h`: follows up to two teams (NHL/NBA/NFL/MLB, ESPN's public API, no key) and
  queues score events. `src/sharks.h` holds the shows for every league, plus Sharks extras and
  hockey pong. `src/knicks.h` holds the tiered basketball shows and the scoreboard game screen.
- `src/flights.h`: Flights mode, the nearest plane overhead (adsb.fi / adsb.lol, adsbdb).
- `web/index.html`: the control page. `tools/gzip_webpage.py` gzips it into `src/webpage.h` on
  every build, so edit the HTML, not the header.
- `tools/`: `set-wifi.py`, `teams.py` (refreshes the team list in the page), `setup-qr.py`
  (printable Wi-Fi QR sticker), `flight-map.py` (local map of what Flights mode sees; needs a
  CARTO key in the gitignored `carto.key`).

## Traps already hit

- Fetch tasks run on core 0 at priority 0 with HTTP timeouts under 5 s. Higher priority or longer
  timeouts starve the idle task, and the watchdog reboots the board.
- `time_t` is 32-bit. Average timestamps as `a + (b - a) / 2`, not `(a + b) / 2`.
- The web server and OTA run in their own core-0 task so a slow client can't freeze animations.
  Keep it that way.
- adsb.lol rejects the default ESP32 user agent. `fetchJson` sets its own.
- Avoid mDNS. It crashed busy boards on LANs full of Apple devices, so use the IP.
