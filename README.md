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
3. Optional: home Wi-Fi, either from **Setup → Home Wi-Fi** on the page, or over USB with `python3 tools/set-wifi.py "Network name"` (hidden password prompt). The board restarts, joins your network and scrolls its IP across the grid; http://ledgrid.local usually works too. The LED-Grid network stays up as a fallback, and if your Wi-Fi can't be joined within 20 s it gives up until the next restart.

Effects: Solid, Rainbow, Plasma, Fire, Matrix, Twinkle, Life, Ripple, Lava lamp, Radar, Hypnosis, Tunnel, Starfield, Metaballs, Kaleidoscope, Bounce, Pinwheel, Raindrops, scrolling Text, Draw (paint pixels or upload an image), Layout test. Settings and drawings survive reboots.

Change the data pin, AP name and AP password at the top of `src/main.cpp`.

## Wireless updates

Once the board is on your Wi-Fi: `pio run -e ota -t upload` (address set in `platformio.ini`).
The password lives in `ota.ini`, which is gitignored. To set it up on a fresh checkout, write a new random one there and flash once over USB.
