// LED Grid: 16x16 WS2812B panel on an ESP32, controlled from a browser.
//
// Wi-Fi: the board always runs its own network (AP_SSID below) at
// http://192.168.4.1. Save your home Wi-Fi from the page's Setup panel and it
// also joins that network; the IP it gets scrolls across the grid at boot and
// http://ledgrid.local usually works too.
//
// Every effect draws into frame[] in logical order (index y*W + x, x to the
// right, y down). XY() maps that onto the panel's physical wiring, which the
// Setup panel can change without reflashing.

#include <Arduino.h>
#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>

#include "font5x7.h"
#include "webpage.h"

// ---- Hardware ---------------------------------------------------------------
#define DATA_PIN 13        // the pin labelled D13
constexpr uint8_t W = 16;
constexpr uint8_t H = 16;
constexpr uint16_t NUM_LEDS = W * H;

// ---- Network ----------------------------------------------------------------
static const char *AP_SSID = "LED-Grid";
static const char *AP_PASS = "ledgrid123";   // WPA2 needs at least 8 characters
static const char *HOSTNAME = "ledgrid";
constexpr uint32_t STA_TIMEOUT_MS = 20000;   // give up on home Wi-Fi after this

// ---- Modes and palettes -----------------------------------------------------
enum Mode : uint8_t {
  M_SOLID, M_RAINBOW, M_PLASMA, M_FIRE, M_MATRIX, M_TWINKLE, M_LIFE,
  M_RIPPLE, M_LAVA, M_RADAR, M_HYPNO, M_TUNNEL, M_STARS, M_METABALLS,
  M_KALEIDO, M_BOUNCE, M_PINWHEEL, M_RAIN, M_TEXT, M_DRAW, M_LAYOUT, M_COUNT
};
static const char *MODE_NAMES[M_COUNT] = {
  "Solid", "Rainbow", "Plasma", "Fire", "Matrix", "Twinkle", "Life",
  "Ripple", "Lava lamp", "Radar", "Hypnosis", "Tunnel", "Starfield", "Metaballs",
  "Kaleidoscope", "Bounce", "Pinwheel", "Raindrops", "Text", "Draw", "Layout test"
};

static const char *PALETTE_NAMES[] = {"Rainbow", "Party", "Lava", "Ocean", "Forest", "Heat", "Cloud"};
constexpr uint8_t PALETTE_COUNT = sizeof(PALETTE_NAMES) / sizeof(PALETTE_NAMES[0]);

static CRGBPalette16 paletteFor(uint8_t i) {
  switch (i) {
    case 1: return PartyColors_p;
    case 2: return LavaColors_p;
    case 3: return OceanColors_p;
    case 4: return ForestColors_p;
    case 5: return HeatColors_p;
    case 6: return CloudColors_p;
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
  uint16_t maxMilliamps = 500;     // safe on USB power; raise in Setup with a real supply
  String staSsid;
  String staPass;
} S;

Preferences prefs;
CRGB leds[NUM_LEDS];
CRGB frame[NUM_LEDS];    // what the effects draw, logical order
CRGB canvas[NUM_LEDS];   // the Draw mode picture, logical order
WebServer server(80);

uint32_t vt = 0;               // "virtual" time in ms, scaled by the speed slider
uint32_t saveAt = 0;           // when to write settings to flash (0 = nothing pending)
bool canvasDirty = false;

String noticeText;             // a one-off message scrolled over the effect (the IP)
uint32_t noticeStart = 0;

bool staTrying = false;
bool staUp = false;
uint32_t staStarted = 0;

// ---- Helpers ----------------------------------------------------------------
static inline CRGB &px(int x, int y) { return frame[y * W + x]; }

static uint16_t XY(uint8_t x, uint8_t y) {
  if (S.flipX) x = W - 1 - x;
  if (S.flipY) y = H - 1 - y;
  uint8_t line = S.columns ? x : y;       // which wired row/column
  uint8_t along = S.columns ? y : x;      // position along it
  uint8_t len = S.columns ? H : W;
  if (S.serpentine && (line & 1)) along = len - 1 - along;
  return line * len + along;
}

static CRGB colorOf(uint32_t rgb) { return CRGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

// Fires once every `interval` ms of virtual time.
static bool every(uint32_t &last, uint32_t interval) {
  if (vt - last < interval) return false;
  last = vt;
  return true;
}

static void markDirty() { saveAt = millis() + 2000; }

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
    for (int x = 0; x < W; x++) px(x, y) = CHSV(base + (x + y) * 8, 255, 255);
}

static void fxPlasma() {
  CRGBPalette16 pal = paletteFor(S.palette);
  uint16_t t = vt / 6;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int dx = x * 2 - 15, dy = y * 2 - 15;
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
  CRGBPalette16 pal = S.palette == 0 ? CRGBPalette16(HeatColors_p) : paletteFor(S.palette);
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
  for (int i = 0; i < NUM_LEDS; i++) frame[i].fadeToBlackBy(40);
  for (int x = 0; x < W; x++) {
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
  for (int n = 0; n < 2; n++)
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
  int cx = 15 + (sin8(vt / 40) - 128) / 12;     // centre drifts around, in half-pixels
  int cy = 15 + (cos8(vt / 53) - 128) / 12;
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
      float a = atan2f(y - 7.5f, x - 7.5f);                 // -pi..pi
      uint8_t angle = (uint8_t)((a + PI) * (128.0f / PI));
      uint8_t behind = sweep - angle;                       // wraps, by design
      uint8_t b = behind < 160 ? 255 - behind * 255 / 160 : 0;
      px(x, y) = c;
      px(x, y).nscale8(scale8(b, b));
    }
}

// Angle (0-255 = a full turn) and distance (16 per pixel) of every pixel from
// the centre, for the radial effects. Filled once in setup().
static uint8_t ANG[NUM_LEDS], DIST[NUM_LEDS];

static void initPolar() {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float dx = x - 7.5f, dy = y - 7.5f;
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
      uint8_t d = max(abs(x * 2 - 15), abs(y * 2 - 15));     // 1..15, square rings
      uint8_t band = sin8(d * 24 - vt / 3);
      px(x, y) = ColorFromPalette(pal, d * 8 - vt / 12, scale8(band, band));
    }
}

static void fxStars() {
  constexpr int N = 24;
  static float r[N], a[N], v[N];
  static uint8_t hue[N];
  static bool init = false;
  static uint32_t last = 0;
  auto respawn = [](int i) { r[i] = random8(5) / 4.0f; a[i] = random16() / 65536.0f * TWO_PI; v[i] = 0.04f + random8() / 1500.0f; hue[i] = random8(); };
  if (!init) { for (int i = 0; i < N; i++) { respawn(i); r[i] = random8(110) / 10.0f; } init = true; }
  if (!every(last, 16)) return;
  for (int i = 0; i < NUM_LEDS; i++) frame[i].fadeToBlackBy(60);
  CRGBPalette16 pal = paletteFor(S.palette);
  for (int i = 0; i < N; i++) {
    r[i] += v[i] * (1 + r[i] * 0.35f);          // faster as they near the edge
    int x = lroundf(7.5f + cosf(a[i]) * r[i]), y = lroundf(7.5f + sinf(a[i]) * r[i]);
    if (x < 0 || x >= W || y < 0 || y >= H) { respawn(i); continue; }
    uint8_t b = min(255, (int)(r[i] * 40));
    px(x, y) += S.palette == 0 ? CRGB(b, b, b) : ColorFromPalette(pal, hue[i], b);
  }
}

static void fxMetaballs() {
  CRGBPalette16 pal = S.palette == 0 ? CRGBPalette16(LavaColors_p) : paletteFor(S.palette);
  float bx[3], by[3];
  for (int k = 0; k < 3; k++) {
    bx[k] = 7.5f + 6.5f * sinf(vt / (900.0f + k * 370) + k * 2.1f);
    by[k] = 7.5f + 6.5f * cosf(vt / (1100.0f + k * 290) + k * 1.3f);
  }
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float sum = 0;
      for (int k = 0; k < 3; k++) {
        float dx = x - bx[k], dy = y - by[k];
        sum += 9.0f / (dx * dx + dy * dy + 1.0f);
      }
      px(x, y) = ColorFromPalette(pal, (uint8_t)min(240.0f, sum * 80), sum > 0.6f ? 255 : (uint8_t)(sum * 425));
    }
}

// Noise in one eighth of the square, mirrored into all eight.
static void fxKaleido() {
  CRGBPalette16 pal = paletteFor(S.palette);
  uint32_t z = vt / 2;
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
  constexpr int N = 4;
  static float x[N], y[N], vx[N], vy[N];
  static bool init = false;
  static uint32_t last = 0;
  if (!init) {
    for (int i = 0; i < N; i++) { x[i] = random8(16); y[i] = random8(16); vx[i] = (random8(2) ? 1 : -1) * (0.15f + random8() / 1000.0f); vy[i] = (random8(2) ? 1 : -1) * (0.12f + random8() / 1000.0f); }
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
  constexpr int N = 4;
  static float cx[N], cy[N], r[N] = {99, 99, 99, 99};
  static uint8_t hue[N];
  static uint32_t last = 0;
  if (!every(last, 16)) return;
  for (int i = 0; i < NUM_LEDS; i++) frame[i].fadeToBlackBy(50);
  CRGBPalette16 pal = paletteFor(S.palette);
  for (int k = 0; k < N; k++) {
    r[k] += 0.18f;
    if (r[k] > 12 && random8() < 6) { cx[k] = random8(16); cy[k] = random8(16); r[k] = 0; hue[k] = random8(); }
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

typedef void (*Effect)();
static const Effect EFFECTS[M_COUNT] = {
  fxSolid, fxRainbow, fxPlasma, fxFire, fxMatrix, fxTwinkle, fxLife,
  fxRipple, fxLava, fxRadar, fxHypno, fxTunnel, fxStars, fxMetaballs,
  fxKaleido, fxBounce, fxPinwheel, fxRain, fxText, fxDraw, fxLayout
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
  S.maxMilliamps = constrain(prefs.getUShort("ma", S.maxMilliamps), 200, 20000);
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
  prefs.putUShort("ma", S.maxMilliamps);
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

static String stateJson() {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%06X", (unsigned)S.color);
  String j = "{";
  j += "\"mode\":" + String(S.mode);
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
  j += ",\"maxMilliamps\":" + String(S.maxMilliamps);
  j += ",\"modes\":[";
  for (int i = 0; i < M_COUNT; i++) j += String(i ? "," : "") + "\"" + MODE_NAMES[i] + "\"";
  j += "],\"palettes\":[";
  for (int i = 0; i < PALETTE_COUNT; i++) j += String(i ? "," : "") + "\"" + PALETTE_NAMES[i] + "\"";
  j += "],\"wifi\":{\"ap\":\"" + String(AP_SSID) + "\",\"apIp\":\"" + WiFi.softAPIP().toString() + "\"";
  j += ",\"ssid\":\"" + jsonEscape(S.staSsid) + "\"";
  j += ",\"connected\":" + String(staUp ? "true" : "false");
  j += ",\"ip\":\"" + (staUp ? WiFi.localIP().toString() : String("")) + "\"}";
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
  }
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
  if (body.length() != NUM_LEDS * 6) { server.send(400, "text/plain", "expected 1536 hex chars"); return; }
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

static void setupWeb() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/state", HTTP_GET, [] { server.send(200, "application/json", stateJson()); });
  server.on("/api/set", HTTP_POST, handleSet);
  server.on("/api/frame", HTTP_GET, [] { server.send(200, "text/plain", toHex(frame)); });
  server.on("/api/canvas", HTTP_GET, [] { server.send(200, "text/plain", toHex(canvas)); });
  server.on("/api/canvas", HTTP_POST, handleCanvasPost);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.onNotFound([] { server.sendHeader("Location", "/"); server.send(302); });
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

// Wireless updates: `pio run -e ota -t upload`. The grid shows a progress bar.
static void setupOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setMdnsEnabled(false);    // pollWifi() runs mDNS itself
  ArduinoOTA.onStart([] { FastLED.clear(true); });
  ArduinoOTA.onProgress([](unsigned done, unsigned total) {
    static uint8_t shown = 255;
    uint8_t lit = (uint32_t)done * NUM_LEDS / total;
    if (lit == shown) return;
    shown = lit;
    for (uint16_t i = 0; i < NUM_LEDS; i++) leds[XY(i % W, i / W)] = i < lit ? CRGB(0, 40, 80) : CRGB::Black;
    FastLED.setBrightness(255);
    FastLED.show();
  });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", e); });
  ArduinoOTA.begin();
}

static void showNotice(const String &s) { noticeText = s; noticeStart = millis(); }

static void pollWifi() {
  if (!staTrying) return;
  if (!staUp && WiFi.status() == WL_CONNECTED) {
    staUp = true;
    String ip = WiFi.localIP().toString();
    Serial.printf("Joined \"%s\": http://%s  (or http://%s.local)\n", S.staSsid.c_str(), ip.c_str(), HOSTNAME);
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
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

// ---- Main -------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nLED Grid");
  loadSettings();

  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, S.maxMilliamps);
  FastLED.setBrightness(S.brightness);
  FastLED.clear(true);

  initPolar();
  setupWifi();
  setupWeb();
  setupOta();
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  pollWifi();
  pollSerial();

  static uint32_t lastFrame = 0;
  uint32_t now = millis();
  if (now - lastFrame < 16) return;   // ~60 fps
  vt += (now - lastFrame) * S.speed / 50;
  lastFrame = now;

  EFFECTS[S.mode]();

  // Scroll the notice (e.g. the new IP) twice over everything, then drop it.
  const CRGB *out = frame;
  static CRGB noticeBuf[NUM_LEDS];
  if (noticeText.length()) {
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

  for (uint8_t y = 0; y < H; y++)
    for (uint8_t x = 0; x < W; x++) leds[XY(x, y)] = out[y * W + x];
  FastLED.setBrightness(S.brightness);
  FastLED.show();

  if (saveAt && (int32_t)(millis() - saveAt) >= 0) { saveAt = 0; saveSettings(); }
}
