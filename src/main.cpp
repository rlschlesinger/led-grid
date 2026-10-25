// LED Grid: 16x16 WS2812B panels on an ESP32, controlled from a browser. One
// panel by default; build with -D GRID_W=32 for two panels chained side by side.
//
// Wi-Fi: the board always runs its own network (AP_SSID below) at
// http://192.168.4.1. Save your home Wi-Fi from the page's Setup panel and it
// also joins that network; the IP it gets scrolls across the grid at boot.
//
// Every effect draws into frame[] in logical order (index y*W + x, x to the
// right, y down). XY() maps that onto the panel's physical wiring, which the
// Setup panel can change without reflashing.

#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <algorithm>
#include <vector>

#include "font5x7.h"
#include "webpage.h"

// ---- Hardware ---------------------------------------------------------------
#define DATA_PIN 13        // the pin labelled D13
#ifndef GRID_W
#define GRID_W 16          // 32 = two panels chained side by side (env:wide)
#endif
constexpr uint8_t PANEL = 16;      // each physical panel is 16x16
constexpr uint8_t W = GRID_W;
constexpr uint8_t H = 16;
constexpr uint16_t NUM_LEDS = W * H;
constexpr uint8_t PANELS = W / PANEL;
constexpr float CX = (W - 1) / 2.0f, CY = (H - 1) / 2.0f;   // centre, in pixels
constexpr int OX = (W - PANEL) / 2;   // shifts a 16-wide picture to the middle

// ---- Network ----------------------------------------------------------------
static const char *AP_SSID = "LED-Grid";
static const char *AP_PASS = "ledgrid123";   // WPA2 needs at least 8 characters
static const char *HOSTNAME = "ledgrid";
constexpr uint32_t STA_TIMEOUT_MS = 20000;   // give up on home Wi-Fi after this

// ---- Modes and palettes -----------------------------------------------------
enum Mode : uint8_t {
  M_SOLID, M_RAINBOW, M_PLASMA, M_FIRE, M_MATRIX, M_TWINKLE, M_LIFE,
  M_RIPPLE, M_LAVA, M_RADAR, M_HYPNO, M_TUNNEL, M_STARS, M_METABALLS,
  M_KALEIDO, M_BOUNCE, M_PINWHEEL, M_RAIN, M_PONG, M_COURT, M_TEXT, M_DRAW, M_LAYOUT, M_FLIGHTS, M_COUNT
};
static const char *MODE_NAMES[M_COUNT] = {
  "Solid", "Rainbow", "Plasma", "Fire", "Matrix", "Twinkle", "Life",
  "Ripple", "Lava lamp", "Radar", "Hypnosis", "Tunnel", "Starfield", "Metaballs",
  "Kaleidoscope", "Bounce", "Pinwheel", "Raindrops", "Pong", "Scoreboard", "Text", "Draw", "Layout test", "Flights"
};

// The animations the idle auto-cycle picks from (the page can leave some out).
static const uint8_t ROTATION[] = {
  M_RAINBOW, M_PLASMA, M_FIRE, M_MATRIX, M_TWINKLE, M_LIFE, M_RIPPLE, M_LAVA, M_RADAR,
  M_HYPNO, M_TUNNEL, M_STARS, M_METABALLS, M_KALEIDO, M_BOUNCE, M_PINWHEEL, M_RAIN,
};
constexpr uint8_t ROTATION_LEN = sizeof(ROTATION);

static const char *PALETTE_NAMES[] = {"Rainbow", "Party", "Lava", "Ocean", "Forest", "Heat", "Cloud", "Halloween"};
constexpr uint8_t PAL_HALLOWEEN = 7;

// Purple, orange and green in bands that repeat twice, so every effect gets
// all three whichever slice of the palette it uses (noise effects mostly use
// the middle), while neighbouring entries still blend smoothly.
static const CRGBPalette16 HalloweenColors_p(
  CRGB(60, 0, 120), CRGB(130, 20, 220), CRGB(255, 80, 0), CRGB(255, 140, 20),
  CRGB(40, 200, 0), CRGB(120, 255, 20), CRGB(90, 0, 160), CRGB(150, 40, 240),
  CRGB(230, 60, 0), CRGB(255, 120, 10), CRGB(30, 160, 0), CRGB(90, 230, 0),
  CRGB(70, 0, 140), CRGB(140, 30, 230), CRGB(255, 100, 0), CRGB(60, 0, 120));

// Fire needs a palette ordered by heat. In Halloween: "witch fire", black to
// purple to toxic green to pale green.
static const CRGBPalette16 WitchFire_p(
  CRGB(0, 0, 0), CRGB(10, 0, 20), CRGB(30, 0, 60), CRGB(60, 0, 110),
  CRGB(90, 0, 160), CRGB(110, 20, 190), CRGB(70, 60, 120), CRGB(30, 120, 40),
  CRGB(20, 170, 0), CRGB(40, 210, 0), CRGB(80, 240, 0), CRGB(120, 255, 20),
  CRGB(160, 255, 60), CRGB(190, 255, 110), CRGB(220, 255, 170), CRGB(240, 255, 210));
constexpr uint8_t PALETTE_COUNT = sizeof(PALETTE_NAMES) / sizeof(PALETTE_NAMES[0]);

static CRGBPalette16 paletteFor(uint8_t i) {
  switch (i) {
    case 1: return PartyColors_p;
    case 2: return LavaColors_p;
    case 3: return OceanColors_p;
    case 4: return ForestColors_p;
    case 5: return HeatColors_p;
    case 6: return CloudColors_p;
    case PAL_HALLOWEEN: return HalloweenColors_p;
    default: return RainbowColors_p;
  }
}

// ---- Settings (saved to flash) ----------------------------------------------
struct Settings {
  uint8_t mode = M_PLASMA;
  uint8_t brightness = 48;
  uint8_t speed = 50;             // 1..100
  uint32_t color = 0xFF5000;
  uint8_t palette = 0;
  String text = "HELLO";
  uint8_t textSize = 2;           // 1 = 7 px tall, 2 = 14 px tall
  bool serpentine = true;
  bool columns = false;           // wiring runs down columns instead of along rows
  bool flipX = false;
  bool flipY = false;
  bool swapPanels = false;        // two panels: the data wire enters the right-hand one
  // Teams followed, as "league:espnId:abbr:color1:color2" ("" = none). The
  // defaults are the Sharks and Knicks, which this grid was built for.
  String team[2] = {"nhl:18:SJ:00a0af:ea5a00", "nba:18:NY:0055c8:f56e14"};
  bool gamePong = true;           // the game screen (pong / scoreboard) takes over while a game is live
  bool clockCard = true;          // date/time/weather/sky card on the quarter hour
  bool autoCycle = true;          // rotate through the animations between cards
  uint8_t cycleMins = 3;          // minutes per animation
  uint32_t cycleMask = 0xFFFFFFFF;   // which ROTATION entries play (bit i = ROTATION[i])
  float lat = 39.63f, lon = -119.66f;   // weather and flights: Sparks, NV by default, rounded
  String city = "Sparks";
  uint16_t maxMilliamps = 500;     // safe on USB power; raise in Setup with a real supply
  bool power = true;              // off = dark, apart from game shows
  bool schedOn = false;           // follow the on/off schedule (schedule.h)
  // weekday on, weekday off, weekend on, weekend off: t:<minute> | sr:<offset> | ss:<offset>
  String sched = "t:420,t:1380,t:480,t:0";
  String mqttHost;                // Home Assistant's MQTT broker ("" = not used)
  uint16_t mqttPort = 1883;
  String mqttUser;
  String mqttPass;
  String staSsid;
  String staPass;
} S;

Preferences prefs;
CRGB leds[NUM_LEDS];
CRGB frame[NUM_LEDS];    // what the effects draw, logical order
CRGB canvas[NUM_LEDS];   // the Draw mode picture, logical order
CRGB shown[NUM_LEDS];    // what the panel is showing right now, for the live preview
WebServer server(80);

uint32_t vt = 0;               // "virtual" time in ms, scaled by the speed slider
uint32_t saveAt = 0;           // when to write settings to flash (0 = nothing pending)
volatile uint32_t stateVersion = 1;
// Least free stack each task has had, in bytes: web, games, ambient, flights, mqtt.
volatile uint32_t stackLow[5];
#define NOTE_STACK(i) (stackLow[i] = uxTaskGetStackHighWaterMark(nullptr))   // bumped on every change, so Home Assistant hears about it
bool canvasDirty = false;

String noticeText;             // a one-off message scrolled over the effect (the IP)
uint32_t noticeStart = 0;

bool staTrying = false;
bool staUp = false;
uint32_t staStarted = 0;

// ---- Helpers ----------------------------------------------------------------
static inline CRGB &px(int x, int y) { return frame[y * W + x]; }

// Panels are chained left to right (or right to left with swapPanels); the
// wiring settings apply within each panel, since they're identical panels.
static uint16_t XY(uint8_t x, uint8_t y) {
  uint8_t panel = x / PANEL;
  if (S.swapPanels) panel = PANELS - 1 - panel;
  x %= PANEL;
  if (S.flipX) x = PANEL - 1 - x;
  if (S.flipY) y = H - 1 - y;
  uint8_t line = S.columns ? x : y;       // which wired row/column
  uint8_t along = S.columns ? y : x;      // position along it
  uint8_t len = S.columns ? H : PANEL;
  if (S.serpentine && (line & 1)) along = len - 1 - along;
  return panel * PANEL * H + line * len + along;
}

static CRGB colorOf(uint32_t rgb) { return CRGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

// Fires once every `interval` ms of virtual time.
static bool every(uint32_t &last, uint32_t interval) {
  if (vt - last < interval) return false;
  last = vt;
  return true;
}

static void markDirty() { saveAt = millis() + 2000; stateVersion++; }

// ---- Text -------------------------------------------------------------------
static int textWidth(const String &s, uint8_t scale) { return s.length() * 6 * scale; }

static void drawText(CRGB *buf, const String &s, int x0, int y0, uint8_t scale, CRGB c) {
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t ch = (uint8_t)s[i];
    for (uint8_t col = 0; col < 5; col++) {
      uint8_t bits = pgm_read_byte(&FONT5X7[ch * 5 + col]);
      for (uint8_t row = 0; row < 7; row++) {
        if (!(bits & (1 << row))) continue;
        for (uint8_t dx = 0; dx < scale; dx++)
          for (uint8_t dy = 0; dy < scale; dy++) {
            int x = x0 + (i * 6 + col) * scale + dx;
            int y = y0 + row * scale + dy;
            if (x >= 0 && x < W && y >= 0 && y < H) buf[y * W + x] = c;
          }
      }
    }
  }
}

// ---- Effects ----------------------------------------------------------------
static void fxSolid() { fill_solid(frame, NUM_LEDS, colorOf(S.color)); }

static void fxRainbow() {
  uint8_t base = vt / 12;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      px(x, y) = S.palette ? ColorFromPalette(paletteFor(S.palette), base + (x + y) * 8) : CRGB(CHSV(base + (x + y) * 8, 255, 255));
}

static void fxPlasma() {
  CRGBPalette16 pal = paletteFor(S.palette);
  uint16_t t = vt / 6;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int dx = x * 2 - (W - 1), dy = y * 2 - (H - 1);
      uint8_t dist = sqrt16(dx * dx + dy * dy) * 8;
      uint8_t v = (sin8(x * 18 + t) + sin8(y * 14 - t / 2) +
                   sin8((x + y) * 10 + t / 3) + sin8(dist - t)) / 4;
      px(x, y) = ColorFromPalette(pal, v);
    }
}

static void fxFire() {
  static uint8_t heat[W][H];      // heat[x][0] is the bottom row
  static uint32_t last = 0;
  if (every(last, 25)) {
    for (int x = 0; x < W; x++) {
      for (int y = 0; y < H; y++) heat[x][y] = qsub8(heat[x][y], random8(0, (55 * 10) / H + 2));
      for (int y = H - 1; y >= 2; y--) heat[x][y] = (heat[x][y - 1] + heat[x][y - 2] * 2) / 3;
      if (random8() < 140) heat[x][random8(2)] = qadd8(heat[x][0], random8(160, 255));
    }
  }
  CRGBPalette16 pal = S.palette == 0 ? CRGBPalette16(HeatColors_p)
                    : S.palette == PAL_HALLOWEEN ? CRGBPalette16(WitchFire_p) : paletteFor(S.palette);
  for (int x = 0; x < W; x++)
    for (int y = 0; y < H; y++) px(x, H - 1 - y) = ColorFromPalette(pal, scale8(heat[x][y], 240));
}

static void fxMatrix() {
  static int8_t head[W];
  static uint8_t rate[W], tick[W];
  static bool init = false;
  static uint32_t last = 0;
  if (!init) {
    for (int x = 0; x < W; x++) { head[x] = -random8(H * 2); rate[x] = random8(1, 4); }
    init = true;
  }
  if (!every(last, 30)) return;
  CRGB c = colorOf(S.color);
  if (S.color == 0xFF5000) c = CRGB(0, 255, 70);   // the default orange looks wrong here
  CRGBPalette16 pal = paletteFor(S.palette);
  for (int i = 0; i < NUM_LEDS; i++) frame[i].fadeToBlackBy(40);
  for (int x = 0; x < W; x++) {
    if (S.palette) c = ColorFromPalette(pal, x * 16);   // a palette colours each column
    if (++tick[x] < rate[x]) continue;
    tick[x] = 0;
    if (head[x] >= 0 && head[x] < H) px(x, head[x]) = c;
    head[x]++;
    if (head[x] >= 0 && head[x] < H) px(x, head[x]) = CRGB(180, 255, 180);
    if (head[x] > H + 4) { head[x] = -random8(H); rate[x] = random8(1, 4); }
  }
}

static void fxTwinkle() {
  static uint32_t last = 0;
  if (!every(last, 20)) return;
  CRGBPalette16 pal = paletteFor(S.palette);
  for (int i = 0; i < NUM_LEDS; i++) frame[i].fadeToBlackBy(12);
  for (int n = 0; n < 2 * PANELS; n++)
    if (random8() < 160) frame[random16(NUM_LEDS)] = ColorFromPalette(pal, random8());
}

static void fxLife() {
  static bool cells[H][W];
  static uint8_t hue = 0;
  static uint32_t last = 0, history[8];
  static uint8_t stale = 0;
  static uint16_t gen = 0;
  static bool seeded = false;

  auto seed = [] {
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) cells[y][x] = random8() < 80;
    gen = 0; stale = 0; hue = random8();
  };
  if (!seeded) { seed(); seeded = true; }

  if (every(last, 120)) {
    bool next[H][W];
    uint32_t hash = 2166136261u;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        int n = 0;
        for (int dy = -1; dy <= 1; dy++)
          for (int dx = -1; dx <= 1; dx++)
            if ((dx || dy) && cells[(y + dy + H) % H][(x + dx + W) % W]) n++;
        next[y][x] = n == 3 || (n == 2 && cells[y][x]);
        hash = (hash ^ next[y][x]) * 16777619u;
      }
    memcpy(cells, next, sizeof(cells));
    // Reseed once the board settles into still lifes or short oscillators.
    bool repeat = false;
    for (uint32_t h : history) repeat |= h == hash;
    memmove(history + 1, history, sizeof(history) - sizeof(history[0]));
    history[0] = hash;
    stale = repeat ? stale + 1 : 0;
    if (stale > 12 || ++gen > 600) seed();
    hue++;
  }
  CRGBPalette16 pal = paletteFor(S.palette);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      if (cells[y][x]) px(x, y) = ColorFromPalette(pal, hue + x * 4 + y * 4);
      else px(x, y).fadeToBlackBy(40);
    }
}

static void fxRipple() {
  CRGBPalette16 pal = paletteFor(S.palette);
  int cx = (W - 1) + (sin8(vt / 40) - 128) * PANELS / 12;   // centre drifts around, in half-pixels
  int cy = (H - 1) + (cos8(vt / 53) - 128) / 12;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int dx = x * 2 - cx, dy = y * 2 - cy;
      uint8_t d = sqrt16(dx * dx + dy * dy) * 10;
      uint8_t wave = sin8(d - vt / 4);
      px(x, y) = ColorFromPalette(pal, d / 2 + vt / 30, scale8(wave, wave));
    }
}

static void fxLava() {
  CRGBPalette16 pal = S.palette == 0 ? CRGBPalette16(LavaColors_p) : paletteFor(S.palette);
  uint32_t z = vt / 3;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) px(x, y) = ColorFromPalette(pal, inoise8(x * 40, y * 40, z));
}

static void fxRadar() {
  CRGB c = colorOf(S.color);
  uint8_t sweep = vt / 8;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float a = atan2f(y - CY, x - CX);                     // -pi..pi
      uint8_t angle = (uint8_t)((a + PI) * (128.0f / PI));
      uint8_t behind = sweep - angle;                       // wraps, by design
      uint8_t b = behind < 160 ? 255 - behind * 255 / 160 : 0;
      px(x, y) = S.palette ? ColorFromPalette(paletteFor(S.palette), angle) : c;
      px(x, y).nscale8(scale8(b, b));
    }
}

// Angle (0-255 = a full turn) and distance (16 per pixel) of every pixel from
// the centre, for the radial effects. Filled once in setup().
static uint8_t ANG[NUM_LEDS], DIST[NUM_LEDS];

static void initPolar() {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float dx = x - CX, dy = y - CY;
      ANG[y * W + x] = (uint8_t)((atan2f(dy, dx) + PI) * (128.0f / PI));
      DIST[y * W + x] = (uint8_t)min(255.0f, sqrtf(dx * dx + dy * dy) * 16);
    }
}

// Two-armed spiral turning inward, the colours drifting outward.
static void fxHypno() {
  CRGBPalette16 pal = paletteFor(S.palette);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t arm = ANG[i] * 2 + DIST[i] * 3 - vt / 3;
    uint8_t b = sin8(arm);
    b = scale8(b, b);                       // narrower bright stripes
    frame[i] = ColorFromPalette(pal, DIST[i] - vt / 20, b);
  }
}

// Concentric squares rushing outward.
static void fxTunnel() {
  CRGBPalette16 pal = paletteFor(S.palette);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      // 1..15, square rings (rectangles, stretched to fill a wide screen)
      uint8_t d = max(abs(x * 2 - (W - 1)) / PANELS, abs(y * 2 - (H - 1)));
      uint8_t band = sin8(d * 24 - vt / 3);
      px(x, y) = ColorFromPalette(pal, d * 8 - vt / 12, scale8(band, band));
    }
}

static void fxStars() {
  constexpr int N = 24 * PANELS;
  static float r[N], a[N], v[N];
  static uint8_t hue[N];
  static bool init = false;
  static uint32_t last = 0;
  auto respawn = [](int i) { r[i] = random8(5) / 4.0f; a[i] = random16() / 65536.0f * TWO_PI; v[i] = 0.04f + random8() / 1500.0f; hue[i] = random8(); };
  if (!init) { for (int i = 0; i < N; i++) { respawn(i); r[i] = random8(110 * PANELS) / 10.0f; } init = true; }
  if (!every(last, 16)) return;
  for (int i = 0; i < NUM_LEDS; i++) frame[i].fadeToBlackBy(60);
  CRGBPalette16 pal = paletteFor(S.palette);
  for (int i = 0; i < N; i++) {
    r[i] += v[i] * (1 + r[i] * 0.35f);          // faster as they near the edge
    int x = lroundf(CX + cosf(a[i]) * r[i]), y = lroundf(CY + sinf(a[i]) * r[i]);
    if (x < 0 || x >= W || y < 0 || y >= H) { respawn(i); continue; }
    uint8_t b = min(255, (int)(r[i] * 40));
    px(x, y) += S.palette == 0 ? CRGB(b, b, b) : ColorFromPalette(pal, hue[i], b);
  }
}

static void fxMetaballs() {
  CRGBPalette16 pal = S.palette == 0 ? CRGBPalette16(LavaColors_p) : paletteFor(S.palette);
  constexpr int B = 3 * PANELS;
  float bx[B], by[B];
  for (int k = 0; k < B; k++) {
    bx[k] = CX + (CX - 1) * sinf(vt / (900.0f + k * 370) + k * 2.1f);
    by[k] = CY + (CY - 1) * cosf(vt / (1100.0f + k * 290) + k * 1.3f);
  }
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float sum = 0;
      for (int k = 0; k < B; k++) {
        float dx = x - bx[k], dy = y - by[k];
        sum += 9.0f / (dx * dx + dy * dy + 1.0f);
      }
      px(x, y) = ColorFromPalette(pal, (uint8_t)min(240.0f, sum * 80), sum > 0.6f ? 255 : (uint8_t)(sum * 425));
    }
}

// Noise in one eighth of the square, mirrored into all eight. On a wide
// screen, one quarter mirrored into four.
static void fxKaleido() {
  CRGBPalette16 pal = paletteFor(S.palette);
  uint32_t z = vt / 2;
  if (W != H) {
    for (int y = 0; y < H / 2; y++)
      for (int x = 0; x < W / 2; x++) {
        CRGB c = ColorFromPalette(pal, inoise8(x * 50, y * 50, z) + vt / 40);
        px(W / 2 - 1 - x, H / 2 - 1 - y) = c; px(W / 2 + x, H / 2 - 1 - y) = c;
        px(W / 2 - 1 - x, H / 2 + y) = c; px(W / 2 + x, H / 2 + y) = c;
      }
    return;
  }
  for (int y = 0; y < H / 2; y++)
    for (int x = 0; x <= y; x++) {
      CRGB c = ColorFromPalette(pal, inoise8(x * 50, y * 50, z) + vt / 40);
      int xs[2] = {x, y}, ys[2] = {y, x};
      for (int k = 0; k < 2; k++) {
        int a = xs[k], b = ys[k];
        px(7 - a, 7 - b) = c; px(8 + a, 7 - b) = c;
        px(7 - a, 8 + b) = c; px(8 + a, 8 + b) = c;
      }
    }
}

static void fxBounce() {
  constexpr int N = 4 * PANELS;
  static float x[N], y[N], vx[N], vy[N];
  static bool init = false;
  static uint32_t last = 0;
  if (!init) {
    for (int i = 0; i < N; i++) { x[i] = random8(W); y[i] = random8(H); vx[i] = (random8(2) ? 1 : -1) * (0.15f + random8() / 1000.0f); vy[i] = (random8(2) ? 1 : -1) * (0.12f + random8() / 1000.0f); }
    init = true;
  }
  if (!every(last, 16)) return;
  for (int i = 0; i < NUM_LEDS; i++) frame[i].fadeToBlackBy(45);
  CRGBPalette16 pal = paletteFor(S.palette);
  for (int i = 0; i < N; i++) {
    x[i] += vx[i]; y[i] += vy[i];
    if (x[i] < 0) { x[i] = -x[i]; vx[i] = -vx[i]; }
    if (x[i] > W - 1) { x[i] = 2 * (W - 1) - x[i]; vx[i] = -vx[i]; }
    if (y[i] < 0) { y[i] = -y[i]; vy[i] = -vy[i]; }
    if (y[i] > H - 1) { y[i] = 2 * (H - 1) - y[i]; vy[i] = -vy[i]; }
    px(lroundf(x[i]), lroundf(y[i])) = ColorFromPalette(pal, i * 64 + vt / 50);
  }
}

// Coloured blades turning, twisted further out.
static void fxPinwheel() {
  CRGBPalette16 pal = paletteFor(S.palette);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t blade = ANG[i] * 3 + DIST[i] / 2 - vt / 6;
    frame[i] = ColorFromPalette(pal, blade, qadd8(sin8(blade * 2) / 2, 60));
  }
}

// Rings spreading from random points, like drops on a pond.
static void fxRain() {
  constexpr int N = 4 * PANELS;
  static float cx[N], cy[N], r[N];
  static uint8_t hue[N];
  static uint32_t last = 0;
  static bool init = false;
  if (!init) { for (float &v : r) v = 99; init = true; }
  if (!every(last, 16)) return;
  for (int i = 0; i < NUM_LEDS; i++) frame[i].fadeToBlackBy(50);
  CRGBPalette16 pal = paletteFor(S.palette);
  for (int k = 0; k < N; k++) {
    r[k] += 0.18f;
    if (r[k] > 12 && random8() < 6) { cx[k] = random8(W); cy[k] = random8(H); r[k] = 0; hue[k] = random8(); }
    if (r[k] > 12) continue;
    uint8_t fade = 255 - (uint8_t)(r[k] * 20);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        float d = fabsf(sqrtf((x - cx[k]) * (x - cx[k]) + (y - cy[k]) * (y - cy[k])) - r[k]);
        if (d < 1) px(x, y) += ColorFromPalette(pal, hue[k], scale8(fade, 255 - (uint8_t)(d * 255)));
      }
  }
}

static void fxText() {
  fill_solid(frame, NUM_LEDS, CRGB::Black);
  uint8_t sc = S.textSize;
  int total = textWidth(S.text, sc) + W;
  int offset = (vt / 70) % total;
  drawText(frame, S.text, W - offset, sc == 2 ? 1 : 4, sc, colorOf(S.color));
}

static void fxDraw() { memcpy(frame, canvas, sizeof(frame)); }

// Corners: white top-left, red top-right, green bottom-left, blue bottom-right.
// A yellow dot walks every pixel in reading order (along the top row first).
static void fxLayout() {
  fill_solid(frame, NUM_LEDS, CRGB::Black);
  px(0, 0) = CRGB::White;
  px(W - 1, 0) = CRGB::Red;
  px(0, H - 1) = CRGB::Green;
  px(W - 1, H - 1) = CRGB::Blue;
  uint16_t i = (millis() / 60) % NUM_LEDS;
  frame[i] = CRGB::Yellow;
}

#include "games.h"
#include "sharks.h"
#include "ambient.h"
#include "knicks.h"
#include "flights.h"
#include "schedule.h"
#include "mqtt.h"

typedef void (*Effect)();
static const Effect EFFECTS[M_COUNT] = {
  fxSolid, fxRainbow, fxPlasma, fxFire, fxMatrix, fxTwinkle, fxLife,
  fxRipple, fxLava, fxRadar, fxHypno, fxTunnel, fxStars, fxMetaballs,
  fxKaleido, fxBounce, fxPinwheel, fxRain, fxPong, fxCourt, fxText, fxDraw, fxLayout, fxFlights
};

// ---- Persistence ------------------------------------------------------------
static void loadSettings() {
  prefs.begin("grid", true);
  S.mode = min<uint8_t>(prefs.getUChar("mode", S.mode), M_COUNT - 1);
  S.brightness = prefs.getUChar("bri", S.brightness);
  S.speed = constrain(prefs.getUChar("speed", S.speed), 1, 100);
  S.color = prefs.getUInt("color", S.color);
  S.palette = min<uint8_t>(prefs.getUChar("pal", S.palette), PALETTE_COUNT - 1);
  S.text = prefs.getString("text", S.text);
  S.textSize = constrain(prefs.getUChar("tsize", S.textSize), 1, 2);
  S.serpentine = prefs.getBool("serp", S.serpentine);
  S.columns = prefs.getBool("cols", S.columns);
  S.flipX = prefs.getBool("fx", S.flipX);
  S.flipY = prefs.getBool("fy", S.flipY);
  S.swapPanels = prefs.getBool("swap", S.swapPanels);
  S.team[0] = prefs.getString("t0", S.team[0]);
  S.team[1] = prefs.getString("t1", S.team[1]);
  // Grids from before team picking: alerts switched off meant no team.
  if (!prefs.isKey("t0") && !prefs.getBool("sharks", true)) S.team[0] = "";
  if (!prefs.isKey("t1") && !prefs.getBool("knicks", true)) S.team[1] = "";
  S.cycleMask = prefs.getUInt("cmask", S.cycleMask);
  S.lat = prefs.getFloat("lat", S.lat);
  S.lon = prefs.getFloat("lon", S.lon);
  S.city = prefs.getString("city", S.city);
  S.gamePong = prefs.getBool("gpong", S.gamePong);
  S.clockCard = prefs.getBool("card", S.clockCard);
  S.autoCycle = prefs.getBool("auto", S.autoCycle);
  S.cycleMins = constrain(prefs.getUChar("cyc", S.cycleMins), 1, 15);
  S.maxMilliamps = constrain(prefs.getUShort("ma", S.maxMilliamps), 200, 20000);
  S.power = prefs.getBool("pwr", S.power);
  S.schedOn = prefs.getBool("sch", S.schedOn);
  String sc = prefs.getString("sched", S.sched);
  if (parseSched(sc, sched)) S.sched = sc; else parseSched(S.sched, sched);
  schedPrev = prefs.getChar("spv", -1);
  S.mqttHost = prefs.getString("mqh", "");
  S.mqttPort = prefs.getUShort("mqp", S.mqttPort);
  S.mqttUser = prefs.getString("mqu", "");
  S.mqttPass = prefs.getString("mqpw", "");
  S.staSsid = prefs.getString("ssid", "");
  S.staPass = prefs.getString("pass", "");
  if (prefs.getBytesLength("canvas") == sizeof(canvas)) prefs.getBytes("canvas", canvas, sizeof(canvas));
  prefs.end();
}

static void saveSettings() {
  prefs.begin("grid", false);
  prefs.putUChar("mode", S.mode);
  prefs.putUChar("bri", S.brightness);
  prefs.putUChar("speed", S.speed);
  prefs.putUInt("color", S.color);
  prefs.putUChar("pal", S.palette);
  prefs.putString("text", S.text);
  prefs.putUChar("tsize", S.textSize);
  prefs.putBool("serp", S.serpentine);
  prefs.putBool("cols", S.columns);
  prefs.putBool("fx", S.flipX);
  prefs.putBool("fy", S.flipY);
  prefs.putBool("swap", S.swapPanels);
  prefs.putUShort("ma", S.maxMilliamps);
  prefs.putString("t0", S.team[0]);
  prefs.putString("t1", S.team[1]);
  prefs.putUInt("cmask", S.cycleMask);
  prefs.putFloat("lat", S.lat);
  prefs.putFloat("lon", S.lon);
  prefs.putString("city", S.city);
  prefs.putBool("gpong", S.gamePong);
  prefs.putBool("card", S.clockCard);
  prefs.putBool("auto", S.autoCycle);
  prefs.putUChar("cyc", S.cycleMins);
  prefs.putBool("pwr", S.power);
  prefs.putBool("sch", S.schedOn);
  prefs.putString("sched", S.sched);
  prefs.putChar("spv", schedPrev);
  if (canvasDirty) { prefs.putBytes("canvas", canvas, sizeof(canvas)); canvasDirty = false; }
  prefs.end();
}

// ---- Web API ----------------------------------------------------------------
static String jsonEscape(const String &s) {
  String out;
  for (char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c >= 32) out += c;
  }
  return out;
}

// Restarts by the games watchdog, kept across reboots so a freeze leaves a trace.
static uint32_t dogCount = 0;
static uint8_t dogWhy = 0;        // the last one's reason: 1 = games task stopped, 2 = ESPN went quiet

static void loadDog() {
  Preferences p;
  p.begin("dog", true);
  dogCount = p.getUInt("n", 0);
  dogWhy = p.getUChar("why", 0);
  p.end();
}

static void watchdogRestart(int why) {
  Preferences p;
  p.begin("dog", false);
  p.putUInt("n", dogCount + 1);
  p.putUChar("why", why);
  p.end();
  Serial.printf("Watchdog: games stuck (%d). Restarting.\n", why);
  Serial.flush();
  delay(200);
  ESP.restart();
}

static String stateJson() {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%06X", (unsigned)S.color);
  String j = "{";
  j += "\"w\":" + String(W) + ",\"h\":" + String(H);
  j += ",\"mode\":" + String(S.mode);
  j += ",\"brightness\":" + String(S.brightness);
  j += ",\"speed\":" + String(S.speed);
  j += ",\"color\":\"" + String(hex) + "\"";
  j += ",\"palette\":" + String(S.palette);
  j += ",\"text\":\"" + jsonEscape(S.text) + "\"";
  j += ",\"textSize\":" + String(S.textSize);
  j += ",\"serpentine\":" + String(S.serpentine ? "true" : "false");
  j += ",\"columns\":" + String(S.columns ? "true" : "false");
  j += ",\"flipX\":" + String(S.flipX ? "true" : "false");
  j += ",\"flipY\":" + String(S.flipY ? "true" : "false");
  j += ",\"swapPanels\":" + String(S.swapPanels ? "true" : "false");
  j += ",\"maxMilliamps\":" + String(S.maxMilliamps);
  j += ",\"power\":" + String(S.power ? "true" : "false");
  j += ",\"schedOn\":" + String(S.schedOn ? "true" : "false");
  j += ",\"sched\":\"" + S.sched + "\",\"next\":\"" + schedNext() + "\"";
  j += ",\"mqtt\":" + mqttJson();
  j += ",\"teams\":" + teamsJson();
  j += ",\"gamePong\":" + String(S.gamePong ? "true" : "false");
  j += ",\"clockCard\":" + String(S.clockCard ? "true" : "false");
  j += ",\"autoCycle\":" + String(S.autoCycle ? "true" : "false");
  j += ",\"cycleMins\":" + String(S.cycleMins);
  j += ",\"cycleMask\":" + String(S.cycleMask);
  j += ",\"rotation\":[";
  for (int i = 0; i < ROTATION_LEN; i++) j += String(i ? "," : "") + ROTATION[i];
  j += "]";
  j += ",\"home\":" + ambientJson();
  j += ",\"flight\":" + flightsJson();
  j += ",\"uptime\":" + String(millis() / 1000);
  j += ",\"rssi\":" + String(WiFi.RSSI());
  j += ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"minHeap\":" + String(ESP.getMinFreeHeap()) + ",\"maxBlock\":" + String(ESP.getMaxAllocHeap());
  j += ",\"reset\":" + String((int)esp_reset_reason());
  j += ",\"dog\":{\"n\":" + String(dogCount) + ",\"why\":" + String(dogWhy) + "}";
  j += ",\"stacks\":[";
  for (int i = 0; i < 5; i++) j += String(i ? "," : "") + stackLow[i];
  j += "]";
  j += ",\"modes\":[";
  for (int i = 0; i < M_COUNT; i++) j += String(i ? "," : "") + "\"" + MODE_NAMES[i] + "\"";
  j += "],\"palettes\":[";
  for (int i = 0; i < PALETTE_COUNT; i++) j += String(i ? "," : "") + "\"" + PALETTE_NAMES[i] + "\"";
  j += "],\"wifi\":{\"ap\":\"" + String(AP_SSID) + "\",\"apIp\":\"" + WiFi.softAPIP().toString() + "\"";
  j += ",\"ssid\":\"" + jsonEscape(S.staSsid) + "\"";
  bool joined = WiFi.status() == WL_CONNECTED;
  j += ",\"connected\":" + String(joined ? "true" : "false");
  j += ",\"ip\":\"" + (joined ? WiFi.localIP().toString() : String("")) + "\"}";
  j += "}";
  return j;
}

static bool argBool(const char *name) {
  String v = server.arg(name);
  return v == "1" || v == "true" || v == "on";
}

static void handleSet() {
  if (server.hasArg("mode")) {
    uint8_t m = server.arg("mode").toInt();
    if (m < M_COUNT && m != S.mode) { S.mode = m; fill_solid(frame, NUM_LEDS, CRGB::Black); }
    if (!server.hasArg("autoCycle")) S.autoCycle = false;   // picking an effect by hand stops the rotation
  }
  if (server.hasArg("autoCycle")) S.autoCycle = argBool("autoCycle");
  if (server.hasArg("cycleMins")) S.cycleMins = constrain(server.arg("cycleMins").toInt(), 1, 15);
  if (server.hasArg("brightness")) S.brightness = constrain(server.arg("brightness").toInt(), 0, 255);
  if (server.hasArg("speed")) S.speed = constrain(server.arg("speed").toInt(), 1, 100);
  if (server.hasArg("color")) {
    String c = server.arg("color");
    if (c.startsWith("#")) c.remove(0, 1);
    S.color = strtoul(c.c_str(), nullptr, 16) & 0xFFFFFF;
  }
  if (server.hasArg("palette")) S.palette = constrain(server.arg("palette").toInt(), 0, PALETTE_COUNT - 1);
  if (server.hasArg("text")) {
    String t;
    for (char c : server.arg("text")) if (c >= 32 && c < 127 && t.length() < 120) t += c;
    S.text = t;
  }
  if (server.hasArg("textSize")) S.textSize = constrain(server.arg("textSize").toInt(), 1, 2);
  if (server.hasArg("serpentine")) S.serpentine = argBool("serpentine");
  if (server.hasArg("columns")) S.columns = argBool("columns");
  if (server.hasArg("flipX")) S.flipX = argBool("flipX");
  if (server.hasArg("flipY")) S.flipY = argBool("flipY");
  if (server.hasArg("swapPanels")) S.swapPanels = argBool("swapPanels");
  if (server.hasArg("team0") || server.hasArg("team1")) {
    for (int i = 0; i < 2; i++) {
      String k = "team" + String(i);
      if (!server.hasArg(k)) continue;
      String v = server.arg(k);
      S.team[i] = v.length() < 40 && (v.isEmpty() || parseTeam(v).league) ? v : String("");
    }
    setTeams(S.team);
  }
  if (server.hasArg("cycleMask")) S.cycleMask = strtoul(server.arg("cycleMask").c_str(), nullptr, 10);
  if (server.hasArg("lat") && server.hasArg("lon")) {
    // Rounded to ~1 km: plenty for weather and sunrise, and no street address.
    S.lat = roundf(constrain(server.arg("lat").toFloat(), -90.0f, 90.0f) * 100) / 100;
    S.lon = roundf(constrain(server.arg("lon").toFloat(), -180.0f, 180.0f) * 100) / 100;
    S.city = server.arg("city").substring(0, 40);
    setHome(S.lat, S.lon, S.city);
  }
  if (server.hasArg("gamePong")) S.gamePong = argBool("gamePong");
  if (server.hasArg("power")) setPower(argBool("power"));
  if (server.hasArg("sched")) {
    SchedEv ev[4];
    if (parseSched(server.arg("sched"), ev)) { memcpy(sched, ev, sizeof(sched)); S.sched = server.arg("sched"); schedPrev = -1; }
  }
  if (server.hasArg("schedOn")) setSchedule(argBool("schedOn"));
  if (server.hasArg("clockCard")) S.clockCard = argBool("clockCard");
  if (server.hasArg("maxMilliamps")) {
    S.maxMilliamps = constrain(server.arg("maxMilliamps").toInt(), 200, 20000);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, S.maxMilliamps);
  }
  markDirty();
  server.send(200, "application/json", stateJson());
}

static String toHex(const CRGB *buf) {
  static const char *digits = "0123456789abcdef";
  String s;
  s.reserve(NUM_LEDS * 6);
  for (int i = 0; i < NUM_LEDS; i++)
    for (uint8_t v : {buf[i].r, buf[i].g, buf[i].b}) { s += digits[v >> 4]; s += digits[v & 15]; }
  return s;
}

static void handleCanvasPost() {
  const String &body = server.arg("plain");
  if (body.length() != NUM_LEDS * 6) { server.send(400, "text/plain", "expected " + String(NUM_LEDS * 6) + " hex chars"); return; }
  char b[3] = {0, 0, 0};
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t ch[3];
    for (int k = 0; k < 3; k++) {
      b[0] = body[i * 6 + k * 2]; b[1] = body[i * 6 + k * 2 + 1];
      ch[k] = strtoul(b, nullptr, 16);
    }
    canvas[i] = CRGB(ch[0], ch[1], ch[2]);
  }
  S.mode = M_DRAW;
  canvasDirty = true;
  markDirty();
  server.send(204);
}

static void handleWifi() {
  S.staSsid = server.arg("ssid");
  S.staPass = server.arg("pass");
  prefs.begin("grid", false);
  prefs.putString("ssid", S.staSsid);
  prefs.putString("pass", S.staPass);
  prefs.end();
  server.send(200, "text/plain", S.staSsid.length() ? "Saved. Restarting to join " + S.staSsid + "." : "Forgotten. Restarting.");
  delay(500);
  ESP.restart();
}

// Home Assistant's broker. An empty host stops MQTT; an empty password keeps the saved one.
static void handleMqtt() {
  S.mqttHost = server.arg("host").substring(0, 64);
  S.mqttHost.trim();
  S.mqttPort = server.hasArg("port") ? constrain(server.arg("port").toInt(), 1, 65535) : 1883;
  S.mqttUser = server.arg("user").substring(0, 64);
  if (server.arg("pass").length()) S.mqttPass = server.arg("pass").substring(0, 64);
  if (!S.mqttHost.length()) { S.mqttUser = ""; S.mqttPass = ""; }
  prefs.begin("grid", false);
  prefs.putString("mqh", S.mqttHost);
  prefs.putUShort("mqp", S.mqttPort);
  prefs.putString("mqu", S.mqttUser);
  prefs.putString("mqpw", S.mqttPass);
  prefs.end();
  mqttReconfig = true;
  startMqtt();
  server.send(200, "application/json", stateJson());
}

// Nearby networks for the Wi-Fi picker: strongest first, each name once.
// Scanning takes 2-3 s; it runs on the web task, so the animation carries on.
static void handleScan() {
  int n = WiFi.scanNetworks();
  std::vector<int> order;
  for (int i = 0; i < n; i++) order.push_back(i);
  std::sort(order.begin(), order.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
  String j = "[";
  std::vector<String> seen;
  for (int i : order) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length() || ssid == AP_SSID || std::find(seen.begin(), seen.end(), ssid) != seen.end()) continue;
    seen.push_back(ssid);
    j += String(seen.size() > 1 ? "," : "") + "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + WiFi.RSSI(i) +
         ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    if (seen.size() >= 20) break;
  }
  WiFi.scanDelete();
  server.send(200, "application/json", j + "]");
}

static void setupWeb() {
  server.on("/", HTTP_GET, [] {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char *)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  });
  server.on("/api/state", HTTP_GET, [] { server.send(200, "application/json", stateJson()); });
  server.on("/api/set", HTTP_POST, handleSet);
  server.on("/api/frame", HTTP_GET, [] { server.send(200, "text/plain", toHex(shown)); });
  server.on("/api/canvas", HTTP_GET, [] { server.send(200, "text/plain", toHex(canvas)); });
  server.on("/api/canvas", HTTP_POST, handleCanvasPost);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/restart", HTTP_POST, [] {
    server.send(200, "text/plain", "Restarting.");
    delay(500);
    ESP.restart();
  });
  server.on("/api/mqtt", HTTP_POST, handleMqtt);
  // Preview a team's shows: /api/test?slot=0&kind=goal|against|win|loss
  // (basketball also: swish|three|opp|run|lead|brick).
  server.on("/api/test", HTTP_POST, [] {
    static const char *NAMES[] = {"", "goal", "against", "win", "loss", "swish", "three", "opp", "run", "lead", "brick"};
    int slot = constrain(server.arg("slot").toInt(), 0, SLOTS - 1);
    Team t = teamAt(slot);
    if (!t.league) { server.send(400, "text/plain", "no team in that slot"); return; }
    String k = server.arg("kind");
    ShowKind kind = SHOW_GOAL;
    for (int i = 1; i <= K_BRICK; i++) if (k == NAMES[i]) kind = (ShowKind)i;
    bool nba = t.league == L_NBA;
    if (nba && kind == SHOW_GOAL) kind = K_SWISH;
    if (nba && kind == SHOW_AGAINST) kind = K_OPP;
    if (nba && kind == SHOW_WIN) kind = K_WIN;
    if (nba && kind == SHOW_LOSS) kind = K_LOSS;
    // Plausible numbers for each sport, and points for the kind of score.
    static const int US[L_COUNT] = {0, 3, 104, 24, 5}, THEM[L_COUNT] = {0, 2, 99, 17, 3};   // NHL, NBA, NFL, MLB
    int extra = t.league == L_NFL ? 6 : t.league == L_MLB ? 2 : 1;
    if (server.hasArg("pts")) extra = server.arg("pts").toInt();
    if (kind == K_RUN) extra = 10; else if (kind == K_BRICK) extra = 8; else if (kind == K_SWISH) extra = 2; else if (kind == K_THREE) extra = 3;
    bool bad = kind == SHOW_AGAINST || kind == SHOW_LOSS || kind == K_OPP || kind == K_BRICK || kind == K_LOSS;
    int us = US[t.league], them = THEM[t.league];
    pushEvent(kind, slot, bad ? them : us, bad ? us : them, "OPP", extra);
    server.send(204);
  });
  // A made-up flight passing overhead, on the Flights screen:
  // /api/flight/test?callsign=UAL1234&route=SFO>DEN&type=AIRBUS A320 (Southwest by default).
  server.on("/api/flight/test", HTTP_POST, [] {
    S.mode = M_FLIGHTS;
    S.autoCycle = false;
    markDirty();
    String cs = server.hasArg("callsign") ? server.arg("callsign") : "SWA3091";
    cs.toUpperCase();
    startFlightDemo(cs, server.hasArg("route") ? server.arg("route") : "LAS>RNO",
                    server.hasArg("type") ? server.arg("type") : "BOEING 737-800");
    server.send(204);
  });
  server.on("/api/card/test", HTTP_POST, [] { triggerCard(strtoul(server.arg("at").c_str(), nullptr, 10)); server.send(204); });
  server.on("/api/scan", HTTP_GET, handleScan);
  // Anything else goes to the page. On the setup network that includes the
  // phone's captive-portal check (captive.apple.com, /generate_204...), so
  // joining LED-Grid pops the page open by itself.
  server.onNotFound([] {
    server.sendHeader("Location", "http://" + server.client().localIP().toString() + "/");
    server.send(302);
  });
  server.begin();
}

// ---- Wi-Fi ------------------------------------------------------------------
static void setupWifi() {
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(S.staSsid.length() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);
  Serial.printf("Access point \"%s\" (password %s): http://%s\n", AP_SSID, AP_PASS,
                WiFi.softAPIP().toString().c_str());
  if (S.staSsid.length()) {
    WiFi.begin(S.staSsid.c_str(), S.staPass.c_str());
    staTrying = true;
    staStarted = millis();
    Serial.printf("Joining \"%s\"...\n", S.staSsid.c_str());
  }
}

static volatile bool otaActive = false;   // set while an OTA upload owns the LEDs

// Wireless updates: `pio run -e ota -t upload`. The grid shows a progress bar.
static void setupOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setMdnsEnabled(false);    // no mDNS: it never resolved here, and it's costly on a busy LAN
  ArduinoOTA.onStart([] { otaActive = true; delay(30); FastLED.clear(true); });   // loop() stops drawing
  ArduinoOTA.onEnd([] { otaActive = false; });
  ArduinoOTA.onProgress([](unsigned done, unsigned total) {
    static uint8_t shown = 255;
    uint8_t lit = (uint32_t)done * NUM_LEDS / total;
    if (lit == shown) return;
    shown = lit;
    for (uint16_t i = 0; i < NUM_LEDS; i++) leds[XY(i % W, i / W)] = i < lit ? CRGB(0, 40, 80) : CRGB::Black;
    FastLED.setBrightness(255);
    FastLED.show();
  });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", e); otaActive = false; });
  ArduinoOTA.begin();
}

// While the setup network is up, a DNS server answers every name with the
// grid's own address, so a phone joining it finds the setup page by itself.
static DNSServer dns;
static volatile bool apOn = true;   // the setup network (LED-Grid) is up

// The web server, DNS and OTA run in their own task on core 0, so a slow or
// lossy client can never stall the animation on core 1.
static void webTask(void *) {
  bool dnsUp = false;
  for (;;) {
    NOTE_STACK(0);
    if (apOn != dnsUp) {
      if (apOn) dns.start(53, "*", WiFi.softAPIP());
      else dns.stop();
      dnsUp = apOn;
    }
    if (dnsUp) dns.processNextRequest();
    server.handleClient();
    ArduinoOTA.handle();
    static uint32_t dogAt = 0;
    if (millis() - dogAt > 10000 && !otaActive) {
      dogAt = millis();
      if (int why = gamesStuck()) watchdogRestart(why);
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static void showNotice(const String &s) { noticeText = s; noticeStart = millis(); }

// Once on home Wi-Fi the backup network goes off: running both at once costs
// the ESP32 a lot of dropped packets. If home Wi-Fi is lost for a minute, the
// backup network comes back until it reconnects.
static void setBackupAp(bool on) {
  if (on == apOn) return;
  apOn = on;
  if (on) { WiFi.mode(WIFI_AP_STA); WiFi.softAP(AP_SSID, AP_PASS); }
  else { WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); }
  Serial.printf("Backup network %s\n", on ? "on" : "off");
}

static void pollWifi() {
  if (!staTrying) return;
  bool connected = WiFi.status() == WL_CONNECTED;
  static uint32_t lostSince = 0;
  if (staUp) {
    if (connected) { lostSince = 0; setBackupAp(false); }
    else if (!lostSince) lostSince = millis();
    else {
      if (millis() - lostSince > 60000) setBackupAp(true);
      // Auto-reconnect can stall for good; nudge it every 30 s while lost.
      static uint32_t lastNudge = 0;
      if (millis() - lostSince > 15000 && millis() - lastNudge > 30000) {
        lastNudge = millis();
        WiFi.reconnect();
        Serial.println("Home Wi-Fi lost; reconnecting");
      }
    }
    return;
  }
  if (connected) {
    staUp = true;
    String ip = WiFi.localIP().toString();
    Serial.printf("Joined \"%s\": http://%s\n", S.staSsid.c_str(), ip.c_str());
    showNotice(ip);
  } else if (!staUp && millis() - staStarted > STA_TIMEOUT_MS) {
    // A station that keeps retrying makes the access point hop channels and
    // drop phones, so stop trying until the next boot.
    Serial.println("Couldn't join home Wi-Fi; access point only until restart.");
    staTrying = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    showNotice("NO WIFI");
  }
}

// ---- Serial setup -------------------------------------------------------------
// tools/set-wifi.py sends "WIFI<tab>ssid<tab>password" so home Wi-Fi can be set
// over USB without joining the board's own network.
static void pollSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (line.length() < 200) line += c; continue; }
    if (line.startsWith("WIFI\t")) {
      int tab = line.indexOf('\t', 5);
      if (tab > 5) {
        S.staSsid = line.substring(5, tab);
        S.staPass = line.substring(tab + 1);
        prefs.begin("grid", false);
        prefs.putString("ssid", S.staSsid);
        prefs.putString("pass", S.staPass);
        prefs.end();
        Serial.printf("Saved Wi-Fi \"%s\". Restarting.\n", S.staSsid.c_str());
        Serial.flush();
        delay(200);
        ESP.restart();
      }
    }
    line = "";
  }
}

// ---- Idle auto-cycle --------------------------------------------------------
// Between quarter-hour cards, rotate through every animation (shuffled, no
// repeats until all have played) in the chosen palette, with a quick fade.
// A new one starts right after each card. Not saved to flash: only the
// rotation setting is, so it doesn't wear the flash every few minutes.
static bool cardShowing = false;

static uint8_t nextAutoMode() {
  static uint8_t bag[ROTATION_LEN], left = 0;
  if (!left) {
    for (int i = 0; i < ROTATION_LEN; i++) if (S.cycleMask & (1UL << i)) bag[left++] = ROTATION[i];
    if (!left) bag[left++] = M_PLASMA;   // everything unticked: don't stall
    for (int i = left - 1; i > 0; i--) { int j = random8(i + 1); uint8_t t = bag[i]; bag[i] = bag[j]; bag[j] = t; }
    if (bag[left - 1] == S.mode) { uint8_t t = bag[0]; bag[0] = bag[left - 1]; bag[left - 1] = t; }
  }
  return bag[--left];
}

// Advances the rotation; returns the brightness for the fade (255 = none).
static uint8_t autoCycleStep(uint32_t now, bool pongTime) {
  constexpr uint32_t FADE = 400;
  static uint32_t nextSwitch = 0, fadeOutAt = 0, fadeInAt = 0;
  static bool fadingOut = false, wasCard = false;
  bool cardEnded = wasCard && !cardShowing;
  wasCard = cardShowing;
  if (!S.autoCycle || pongTime) { fadingOut = false; nextSwitch = 0; return 255; }

  auto start = [&](uint32_t t) {
    S.mode = nextAutoMode();
    fill_solid(frame, NUM_LEDS, CRGB::Black);
    fadingOut = false;
    fadeInAt = t;
    nextSwitch = t + S.cycleMins * 60000UL;
  };
  if (cardEnded || nextSwitch == 0) start(now);            // fresh after each card
  else if (!fadingOut && (int32_t)(now - nextSwitch) >= 0) { fadingOut = true; fadeOutAt = now; }
  if (fadingOut) {
    if (now - fadeOutAt >= FADE) start(now);
    else return 255 - (now - fadeOutAt) * 255 / FADE;
  }
  return now - fadeInAt < FADE ? (now - fadeInAt) * 255 / FADE : 255;
}

// ---- Main -------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nLED Grid");
  loadSettings();
  loadDog();

  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, S.maxMilliamps);
  FastLED.setBrightness(S.brightness);
  FastLED.clear(true);

  initPolar();
  setupWifi();
  setupWeb();
  setupOta();
  setupGames(S.team);
  setupAmbient(S.lat, S.lon, S.city);
  setupFlights();
  setupMqtt();
  xTaskCreatePinnedToCore(webTask, "web", 6144, nullptr, 1, nullptr, 0);
}

void loop() {
  if (otaActive) { delay(5); return; }   // the OTA progress bar owns the LEDs
  pollWifi();
  pollSerial();
  schedTick();

  static uint32_t lastFrame = 0;
  uint32_t now = millis();
  if (now - lastFrame < 16) return;   // ~60 fps
  vt += (now - lastFrame) * S.speed / 50;
  lastFrame = now;

  // While a followed team is playing, its game screen runs (shows still
  // interrupt it): pong for hockey, the scoreboard for everything else. If
  // both teams play at once, the first slot's game has the screen.
  int live = S.gamePong ? liveSlot() : -1;
  bool hockey = live >= 0 && teamAt(live).league == L_NHL;
  bool pongTime = live >= 0 && hockey;
  bool courtTime = live >= 0 && !hockey;
  uint8_t cycleLevel = autoCycleStep(now, pongTime || courtTime);
  EFFECTS[pongTime ? M_PONG : courtTime ? M_COURT : S.mode]();

  // No home Wi-Fi: every 45 s, say how to set it up.
  static uint32_t lastHint = 0;
  if (!staUp && !staTrying && !noticeText.length() && now - lastHint > 45000) {
    lastHint = now;
    showNotice("JOIN WIFI " + String(AP_SSID));
  }

  // Scroll the notice (e.g. the new IP) twice over everything, then drop it.
  const CRGB *out = frame;
  static CRGB noticeBuf[NUM_LEDS], showBuf[NUM_LEDS];
  bool showing = renderShow(showBuf, now);
  if (showing) {
    out = showBuf;
  } else if ((cardShowing = renderCard(showBuf, now))) {   // the quarter-hour card
    out = showBuf;
  } else if (noticeText.length()) {
    int total = textWidth(noticeText, 1) + W;
    int offset = (now - noticeStart) / 60;
    if (offset >= total * 2) {
      noticeText = "";
    } else {
      fill_solid(noticeBuf, NUM_LEDS, CRGB::Black);
      drawText(noticeBuf, noticeText, W - offset % total, 4, 1, CRGB::White);
      out = noticeBuf;
    }
  }

  if (out == frame && cycleLevel < 255) {    // fading between auto-cycled animations
    static CRGB faded[NUM_LEDS];
    memcpy(faded, frame, sizeof(faded));
    nscale8(faded, NUM_LEDS, cycleLevel);
    out = faded;
  }
  if (!S.power && !showing) {               // switched off: dark, but game shows still play
    static CRGB dark[NUM_LEDS];
    out = dark;
  }
  memcpy(shown, out, sizeof(shown));
  for (uint8_t y = 0; y < H; y++)
    for (uint8_t x = 0; x < W; x++) leds[XY(x, y)] = out[y * W + x];
  FastLED.setBrightness(showing ? max<uint8_t>(S.brightness, 140) : S.brightness);   // power limit still applies
  FastLED.show();

  if (saveAt && (int32_t)(millis() - saveAt) >= 0) { saveAt = 0; saveSettings(); }
}
