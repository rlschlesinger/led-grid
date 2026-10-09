# LED Grid

16×16 WS2812B panel on a classic ESP32 DevKit (CP2102, USB-C), controlled from a phone over Wi-Fi.

## Wiring

| Panel wire        | Goes to                                   |
|-------------------|-------------------------------------------|
| Red (5V)          | 5 V power supply **+**                    |
| White (GND)       | Power supply **−** *and* an ESP32 `GND` pin |
| Green (data)      | ESP32 `D13`, through a 330 Ω resistor if you have one |
| Extra red/white pair | Same supply + / − (power injection)   |

- The ESP32 must feed the panel's **DIN** end. If the green wire is on a pad marked DOUT, use the 3 wires at the other end of the panel.
- The common ground is required: without it the data signal has no reference and the panel shows garbage or nothing.
- A full-white panel draws up to ~15 A. Use a 5 V supply rated 4 A or more, set **Setup → Power limit** a little under its rating (default 500 mA, which is safe on USB power), and put a 1000 µF capacitor across the supply's + and −.
- To run without USB, also wire supply 5 V → ESP32 `VIN`. Plugging USB in as well, with the grounds shared, is fine for flashing.
- 3.3 V data usually drives these panels. If the panel flickers or shows random colours, add a 74AHCT125 level shifter.

## Flash

    pio run -t upload

## Use

1. Join Wi-Fi **LED-Grid** (password `ledgrid123`) and open http://192.168.4.1.
2. Pick **Layout test** and toggle **Setup → Panel layout** until white is top-left and the yellow dot runs left→right along the top row.
3. Optional: home Wi-Fi, either from **Setup → Home Wi-Fi** on the page, or over USB with `python3 tools/set-wifi.py "Network name"` (hidden password prompt). The board restarts, joins your network and scrolls its IP across the grid. Once joined, the LED-Grid network switches off (running both costs the ESP32 a lot of dropped packets) and comes back if home Wi-Fi is lost for a minute. If your Wi-Fi can't be joined within 20 s at boot, it stays on LED-Grid until the next restart.

Idle: between quarter-hour cards it auto-cycles (shuffled, every 3 min by default, with a fade) through every animation except Shark pong, Solid, Text, Draw and Layout test, in the chosen palette. Picking an effect by hand turns cycling off. Palettes include Halloween (purple/orange/green, with a "witch fire" variant for Fire).

Effects: Solid, Rainbow, Plasma, Fire, Matrix, Twinkle, Life, Ripple, Lava lamp, Radar, Hypnosis, Tunnel, Starfield, Metaballs, Kaleidoscope, Bounce, Pinwheel, Raindrops, scrolling Text, Draw (paint pixels or upload an image), Layout test. Settings and drawings survive reboots.

Change the data pin, AP name and AP password at the top of `src/main.cpp`.

## Wireless updates

Once the board is on your Wi-Fi: `pio run -e ota -t upload` (address set in `platformio.ini`).
The password lives in `ota.ini`, which is gitignored. To set it up on a fresh checkout, write a new random one there and flash once over USB.

## Sharks goal alerts

On home Wi-Fi the board follows the Sharks on api-web.nhle.com. It checks the week's schedule every 2 minutes and polls the live game every 10 seconds; the fetching runs on core 0.
- Sharks goal: teal/white strobe, rings, a fin, "GOAL!", the score. A win gets its own show: a fin crossing to a quickening Jaws pulse, full-screen jaws chomping three times, a white flash, fireworks, "SHARKS WIN!", the score.
- Goal against: the shark swims in and X-rays into a skeleton that sinks with a glowing red eye, then "UGH." and the score.
- Loss: the shark rolls belly-up and floats away, a tombstone reading RIP rises, then "FINAL" and the score.
- During a live game, Shark pong (also a regular effect) shows the real score, and the shows interrupt it.
- Toggles and test buttons are in the Sharks panel on the page. Polling is a few seconds behind the broadcast and can run ahead of a delayed stream.

## Quarter-hour card

At :00, :15, :30 and :45 (skipped during a live Sharks game) it shows the date, then the time with a weather icon and temperature, then the sky: by day, the sun on today's path from sunrise (left) to sunset (right); by night, the moon in its real phase. Weather, sunrise, sunset and the UTC offset come from Open-Meteo for home (Sparks, NV); the sun's height and the moon phase are calculated on the board. Toggle and "Show it now" are on the page. `POST /api/card/test?at=<unix time>` previews any time of day.
