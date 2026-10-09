// The quarter-hour card. Included once, from main.cpp, after sharks.h.
//
// At :00, :15, :30 and :45 (local time) it interrupts the effect for ~18 s:
//   1. the date scrolls by
//   2. the time, a weather icon and the temperature
//   3. by day, the sun on today's path across the sky; by night, the moon in
//      its current phase among twinkling stars
// Skipped while a Sharks or Knicks game is live (their game screens own it).
//
// Data, fetched on core 0 every 15 minutes from Open-Meteo (no key): the
// temperature, conditions, sunrise, sunset and the UTC offset (so DST just
// follows). Home is fixed below, to the ZIP centroid, rounded.
// The sun's height and the moon phase are worked out here.
#pragma once
#include <time.h>

static const float HOME_LAT = 39.63f, HOME_LON = -119.71f;   // Sparks, NV 89436
static const char *HOME_NAME = "Sparks";

struct Ambient {
  bool weather = false;
  float lat = HOME_LAT, lon = HOME_LON;
  int tempF = 0;
  int code = 0;              // WMO weather code
  time_t sunrise = 0, sunset = 0;
  int32_t utcOffset = 0;     // seconds, DST included
};
static Ambient amb;          // written on core 0 under ambLock, copied out to read
static SemaphoreHandle_t ambLock;

static Ambient ambient() {
  xSemaphoreTake(ambLock, portMAX_DELAY);
  Ambient a = amb;
  xSemaphoreGive(ambLock);
  return a;
}

static void ambientTask(void *) {
  bool timeStarted = false;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
    if (!timeStarted) { configTime(0, 0, "pool.ntp.org", "time.google.com"); timeStarted = true; }

    char url[300];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code&daily=sunrise,sunset&timezone=auto"
             "&temperature_unit=fahrenheit&forecast_days=1&timeformat=unixtime",
             amb.lat, amb.lon);
    JsonDocument filter;
    filter["utc_offset_seconds"] = true;
    filter["current"]["temperature_2m"] = true;
    filter["current"]["weather_code"] = true;
    filter["daily"]["sunrise"] = true;
    filter["daily"]["sunset"] = true;
    JsonDocument doc;
    if (fetchJson(url, doc, filter)) {
      xSemaphoreTake(ambLock, portMAX_DELAY);
      amb.utcOffset = doc["utc_offset_seconds"] | 0;
      amb.tempF = lroundf(doc["current"]["temperature_2m"] | 0.0f);
      amb.code = doc["current"]["weather_code"] | 0;
      amb.sunrise = doc["daily"]["sunrise"][0] | 0;
      amb.sunset = doc["daily"]["sunset"][0] | 0;
      amb.weather = true;
      xSemaphoreGive(ambLock);
      vTaskDelay(pdMS_TO_TICKS(15 * 60 * 1000));
    } else {
      vTaskDelay(pdMS_TO_TICKS(60000));
    }
  }
}

static void setupAmbient() {
  ambLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(ambientTask, "ambient", 12288, nullptr, 0, nullptr, 0);   // idle priority: see setupSharks()
}

static String ambientJson() {
  Ambient a = ambient();
  String j = "{\"city\":\"" + String(HOME_NAME) + "\"";
  if (a.weather) j += ",\"temp\":" + String(a.tempF) + ",\"code\":" + String(a.code);
  return j + "}";
}

// ---- Drawing helpers --------------------------------------------------------
static int digitsWidth(const String &s) {          // 3x5 digits, 1 px gaps, ':' is 1 px
  int w = 0;
  for (size_t i = 0; i < s.length(); i++) w += (s[i] == ':' ? 1 : 3) + (i + 1 < s.length() ? 1 : 0);
  return w;
}

static void drawDigits(CRGB *buf, const String &s, int x, int y, CRGB c, bool colon = true) {
  for (size_t i = 0; i < s.length(); i++) {
    char ch = s[i];
    if (ch == ':') {
      if (colon) { buf[(y + 1) * W + x] = c; buf[(y + 3) * W + x] = c; }
      x += 2;
    } else if (ch == '-') {
      for (int k = 0; k < 3; k++) buf[(y + 2) * W + x + k] = c;
      x += 4;
    } else {
      drawDigit(buf, ch - '0', x, y, c);
      x += 4;
    }
  }
}

// 5x5 weather icons. y = sun yellow, c = cloud grey, b = rain blue, w = snow,
// z = lightning, m = moon.
static const char *iconFor(int code, bool day) {
  if (code <= 1) return day ? "y.y.y.yyy.yyyyy.yyy.y.y.y" : "mmm....mm....m...mm.mmm..";
  if (code == 2) return day ? "...y.cc.yyccccycccccccccc" : "...m.cc..mccccmcccccccccc";
  if (code == 3) return ".....cc...cccc.ccccc.....";
  if (code == 45 || code == 48) return "ccccc.....ccccc.....ccccc";
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return ".cc..ccccc.....w.w.w.w.w.";
  if (code >= 95) return ".cc..ccccc...z...zz...z..";
  return ".cc..ccccc.....b.b.b.b.b.";   // drizzle, rain, showers
}

static void drawIcon(CRGB *buf, const char *icon, int x0, int y0) {
  for (int i = 0; i < 25; i++) {
    CRGB c;
    switch (icon[i]) {
      case 'y': c = CRGB(255, 190, 0); break;
      case 'c': c = CRGB(140, 140, 150); break;
      case 'b': c = CRGB(30, 90, 255); break;
      case 'w': c = CRGB(220, 220, 240); break;
      case 'z': c = CRGB(255, 230, 0); break;
      case 'm': c = CRGB(220, 215, 170); break;
      default: continue;
    }
    buf[(y0 + i / 5) * W + x0 + i % 5] = c;
  }
}

static CRGB tempColor(int f) {                      // blue at 20°F, white near 60, orange-red at 100
  if (f <= 60) return blend(CRGB(40, 110, 255), CRGB(230, 230, 230), constrain((f - 20) * 255 / 40, 0, 255));
  return blend(CRGB(230, 230, 230), CRGB(255, 70, 0), constrain((f - 60) * 255 / 40, 0, 255));
}

// Solar elevation in degrees. Solar noon is taken as the midpoint of sunrise
// and sunset, which folds in longitude and the equation of time for free.
static float sunElevation(time_t t, const Ambient &a, int dayOfYear) {
  float decl = 23.44f * sinf(TWO_PI * (284 + dayOfYear) / 365.0f) * DEG_TO_RAD;
  // sunrise + sunset overflows a 32-bit time_t in 2026; add half the gap instead
  float noon = a.sunrise + (a.sunset - a.sunrise) / 2.0f;
  float hourAngle = (t - noon) / 3600.0f * 15.0f * DEG_TO_RAD;
  float lat = a.lat * DEG_TO_RAD;
  return asinf(sinf(lat) * sinf(decl) + cosf(lat) * cosf(decl) * cosf(hourAngle)) * RAD_TO_DEG;
}

// Moon phase 0..1 (0 new, 0.5 full) from a known new moon, 6 Jan 2000 18:14 UTC.
static float moonPhase(time_t t) {
  double days = (t - 947182440.0) / 86400.0;
  double p = fmod(days / 29.530588853, 1.0);
  return p < 0 ? p + 1 : p;
}

// ---- The card's three scenes --------------------------------------------------
static void sunScene(CRGB *buf, time_t now, const Ambient &a, int yday, uint32_t t) {
  const int HORIZON = 12;
  float elev = sunElevation(now, a, yday);
  float peak = max(20.0f, sunElevation(a.sunrise + (a.sunset - a.sunrise) / 2, a, yday));
  auto rowFor = [&](float e) { return HORIZON - 1 - e / peak * 9; };   // peak lands on row 2
  // Sky: warm near the horizon at sunrise/sunset, blue when the sun is high.
  float warm = constrain(1.0f - elev / 15.0f, 0.0f, 1.0f);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      if (y > HORIZON) { buf[y * W + x] = (x + y) % 3 ? CRGB(15, 40, 10) : CRGB(22, 55, 15); continue; }
      CRGB high = blend(CRGB(10, 40, 110), CRGB(60, 25, 70), warm * 255);
      CRGB low = blend(CRGB(70, 130, 200), CRGB(200, 80, 20), warm * 255);
      buf[y * W + x] = blend(high, low, y * 255 / HORIZON);
    }
  // Today's path, dotted, sunrise on the left (east) to sunset on the right (west).
  for (int x = 0; x < W; x += 2) {
    time_t tt = a.sunrise + (a.sunset - a.sunrise) * x / (W - 1);
    int y = lroundf(rowFor(sunElevation(tt, a, yday)));
    if (y >= 0 && y <= HORIZON) buf[y * W + x] += CRGB(45, 40, 25);
  }
  float f = constrain((float)(now - a.sunrise) / (a.sunset - a.sunrise), 0.0f, 1.0f);
  int sx = lroundf(f * (W - 1)), sy = lroundf(rowFor(max(elev, 0.0f)));
  // The sun: a bright plus with rays that twinkle on the diagonals.
  bool rays = (t / 250) % 2;
  for (int dy = -2; dy <= 2; dy++)
    for (int dx = -2; dx <= 2; dx++) {
      int x = sx + dx, y = sy + dy, d = abs(dx) + abs(dy);
      if (x < 0 || x >= W || y < 0 || y > HORIZON) continue;
      if (d <= 1) buf[y * W + x] = d == 0 ? CRGB(255, 255, 200) : CRGB(255, 200, 30);
      else if (rays && abs(dx) == abs(dy)) buf[y * W + x] = CRGB(255, 150, 0);
    }
}

static void moonScene(CRGB *buf, time_t now, uint32_t t) {
  fill_solid(buf, NUM_LEDS, CRGB(2, 2, 10));
  for (int i = 0; i < 14; i++) {                    // stars, each twinkling at its own pace
    uint32_t h = (i + 1) * 2654435761u;
    int x = h % W, y = (h >> 8) % H;
    uint8_t b = 40 + scale8(sin8(t / (4 + i % 5) + h), 140);
    buf[y * W + x] = CRGB(b, b, b + 20);
  }
  uint32_t shoot = t % 3500;                        // a shooting star now and then
  if (shoot < 450) {
    int k = shoot / 30;
    for (int j = 0; j < 4; j++) {
      int x = 15 - k + j, y = k / 2 + j / 2 - 1;
      if (x >= 0 && x < W && y >= 0 && y < H) buf[y * W + x] = CRGB(200, 200, 255).nscale8(255 - j * 60);
    }
  }
  // The moon, r = 6: lit side bright, dark side faint so the whole disc reads.
  float p = moonPhase(now), c = cosf(TWO_PI * p);
  uint8_t glow = 200 + scale8(sin8(t / 8), 55);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      float dx = x - 7.5f, dy = y - 7.5f, r2 = dx * dx + dy * dy;
      if (r2 > 6.6f * 6.6f) continue;
      float w = sqrtf(max(0.0f, 6.6f * 6.6f - dy * dy));
      bool lit = p <= 0.5f ? dx >= w * c : dx <= -w * c;   // waxing lights the right side
      buf[y * W + x] = lit ? CRGB(235, 228, 190).nscale8(glow) : CRGB(18, 20, 36);
    }
}

// ---- The card -----------------------------------------------------------------
static uint32_t cardStart = 0;
static bool cardOn = false;
static int lastQuarter = -1;

static time_t cardAt = 0;    // test only: show the card as if it were this UTC time

static void triggerCard(time_t at = 0) { cardOn = true; cardStart = millis(); cardAt = at; }

// Draws the card into buf. Returns false when it isn't showing.
static bool renderCard(CRGB *buf, uint32_t nowMs) {
  time_t utc = cardOn && cardAt ? cardAt + (millis() - cardStart) / 1000 : time(nullptr);
  if (utc < 1700000000) return false;               // clock not set yet
  Ambient a = ambient();
  time_t local = utc + a.utcOffset;
  struct tm tm;
  gmtime_r(&local, &tm);

  // The schedule runs on the real clock, never a test time, and never restarts
  // a card that's already showing.
  time_t realLocal = time(nullptr) + a.utcOffset;
  struct tm rt;
  gmtime_r(&realLocal, &rt);
  int quarter = rt.tm_hour * 4 + rt.tm_min / 15;
  bool onQuarter = rt.tm_min % 15 == 0;
  if (!cardOn && S.clockCard && onQuarter && quarter != lastQuarter && !(S.sharks && gameLive) && !(S.knicks && knicksLive)) {
    triggerCard();
    utc = time(nullptr);
    local = utc + a.utcOffset;
    gmtime_r(&local, &tm);
  }
  if (onQuarter) lastQuarter = quarter;
  if (!cardOn) return false;

  static const char *DAYS[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const char *MONTHS[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  String date = String(DAYS[tm.tm_wday]) + " " + MONTHS[tm.tm_mon] + " " + tm.tm_mday;
  uint32_t t = nowMs - cardStart;
  uint32_t dateEnd = (textWidth(date, 1) + W) * 45;
  uint32_t clockEnd = dateEnd + 7000, skyEnd = clockEnd + 7000;
  fill_solid(buf, NUM_LEDS, CRGB::Black);

  if (t < dateEnd) {
    drawText(buf, date, W - (int)(t / 45), 4, 1, CRGB(220, 220, 230));
  } else if (t < clockEnd) {
    int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    char hm[6];
    snprintf(hm, sizeof(hm), "%d:%02d", h12, tm.tm_min);
    String s = hm;
    drawDigits(buf, s, (W - digitsWidth(s)) / 2, 1, CRGB(230, 230, 240), tm.tm_sec % 2 == 0);
    if (a.weather) {
      bool day = utc >= a.sunrise && utc < a.sunset;
      String temp = String(a.tempF);
      bool icon = temp.length() <= 2;
      int tw = digitsWidth(temp) + 3, x = icon ? 6 : (W - tw) / 2;
      if (icon) drawIcon(buf, iconFor(a.code, day), 0, 10);
      drawDigits(buf, temp, x, 10, tempColor(a.tempF));
      x += digitsWidth(temp) + 1;
      buf[10 * W + x] = buf[10 * W + x + 1] = buf[11 * W + x] = buf[11 * W + x + 1] = tempColor(a.tempF);   // degree
    }
  } else if (t < skyEnd && a.weather && a.sunset > a.sunrise) {
    uint32_t u = t - clockEnd;
    bool day = utc >= a.sunrise && utc < a.sunset;
    if (day) sunScene(buf, utc, a, tm.tm_yday, u);
    else moonScene(buf, utc, u);
    uint8_t fade = u < 400 ? u * 255 / 400 : u > 6400 ? (7000 - u) * 255 / 600 : 255;
    nscale8(buf, NUM_LEDS, fade);
  } else {
    cardOn = false;
    return false;
  }
  return true;
}
