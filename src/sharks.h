// Sharks goal alerts. Included once, from main.cpp, after the drawing helpers.
//
// A task on core 0 polls api-web.nhle.com: the Sharks' week schedule every
// 2 minutes, and the live game every 10 seconds. Score changes become events
// on a queue. The render loop on core 1 plays each as a full-screen show over
// whatever effect is running:
//   Sharks goal   teal/white strobe, rings, a fin swimming, "GOAL!", the score
//   goal against  the shark swims in and X-rays into a skeleton, "UGH.", the score
//   final         a win: a fin, JAWS chomping, fireworks, "SHARKS WIN!"; or the shark goes belly-up and a tombstone reads RIP, "FINAL"
#pragma once
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static const char *TEAM = "SJS";
static const CRGB TEAL(0, 160, 175);      // Pacific Teal, brightened for LEDs
static const CRGB ORANGE(234, 90, 0);

enum ShowKind : uint8_t { SHOW_NONE, SHOW_GOAL, SHOW_AGAINST, SHOW_WIN, SHOW_LOSS };

struct GameEvent {
  ShowKind kind;
  uint8_t us, them;
  char opp[4];
};

static QueueHandle_t gameEvents;
static SemaphoreHandle_t gameLock;
static String gameStatus = "{\"state\":\"idle\"}";   // JSON for the web page; guarded by gameLock
static volatile bool gameLive = false;                // read by the render loop: play pong
static volatile uint8_t liveUs = 0, liveThem = 0;

// ---- Fetching (core 0) ------------------------------------------------------
static bool fetchJson(const String &url, JsonDocument &doc, const JsonDocument &filter) {
  WiFiClientSecure client;
  client.setInsecure();                    // public scores; skip certificate checks
  HTTPClient http;
  http.useHTTP10(true);                    // no chunked encoding, so the stream parses directly
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // ".../now" redirects to a date
  http.setTimeout(8000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) ok = !deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  else Serial.printf("NHL %s -> HTTP %d\n", url.c_str(), code);
  http.end();
  return ok;
}

static void setStatus(const String &json) {
  xSemaphoreTake(gameLock, portMAX_DELAY);
  gameStatus = json;
  xSemaphoreGive(gameLock);
}

static String currentStatus() {
  xSemaphoreTake(gameLock, portMAX_DELAY);
  String s = gameStatus;
  xSemaphoreGive(gameLock);
  return s;
}

static void pushEvent(ShowKind kind, int us, int them, const char *opp) {
  GameEvent e{kind, (uint8_t)us, (uint8_t)them, {0}};
  strlcpy(e.opp, opp, sizeof(e.opp));
  xQueueSend(gameEvents, &e, 0);
}

static bool isLive(const char *state) { return !strcmp(state, "LIVE") || !strcmp(state, "CRIT"); }
static bool isOver(const char *state) { return !strcmp(state, "FINAL") || !strcmp(state, "OFF"); }

// Follows one live game until it ends (or alerts are switched off). A Wi-Fi
// blip just waits, so Shark pong doesn't drop out mid-game.
static void followGame(uint32_t id, int us, int them, bool home, const String &opp) {
  JsonDocument filter;
  filter["gameState"] = true;
  filter["awayTeam"]["score"] = true;
  filter["homeTeam"]["score"] = true;
  filter["periodDescriptor"]["number"] = true;
  filter["periodDescriptor"]["periodType"] = true;
  filter["clock"]["timeRemaining"] = true;
  filter["clock"]["inIntermission"] = true;
  String url = "https://api-web.nhle.com/v1/gamecenter/" + String(id) + "/landing";
  Preferences p;

  while (S.sharks) {
    JsonDocument doc;
    if (WiFi.status() == WL_CONNECTED && fetchJson(url, doc, filter)) {
      const char *state = doc["gameState"] | "";
      int nowUs = doc[home ? "homeTeam" : "awayTeam"]["score"] | us;
      int nowThem = doc[home ? "awayTeam" : "homeTeam"]["score"] | them;
      if (nowUs > us) pushEvent(SHOW_GOAL, nowUs, nowThem, opp.c_str());
      if (nowThem > them) pushEvent(SHOW_AGAINST, nowUs, nowThem, opp.c_str());
      us = nowUs;          // a goal overturned on review just lowers the score quietly
      them = nowThem;
      liveUs = us;
      liveThem = them;
      gameLive = !isOver(state);

      String period = String(doc["periodDescriptor"]["periodType"] | "REG") == "REG"
                          ? "P" + String(doc["periodDescriptor"]["number"] | 1)
                          : String(doc["periodDescriptor"]["periodType"] | "");
      String clock = doc["clock"]["inIntermission"] | false ? "INT" : String(doc["clock"]["timeRemaining"] | "");
      setStatus("{\"state\":\"" + String(isOver(state) ? "final" : "live") + "\",\"us\":" + us + ",\"them\":" + them +
                ",\"opp\":\"" + opp + "\",\"home\":" + (home ? "true" : "false") +
                ",\"period\":\"" + period + "\",\"clock\":\"" + clock + "\"}");

      if (isOver(state)) {
        // Remember the game so a reboot doesn't replay the result.
        p.begin("sharks", false);
        if (p.getUInt("done", 0) != id) {
          p.putUInt("done", id);
          pushEvent(us > them ? SHOW_WIN : SHOW_LOSS, us, them, opp.c_str());
        }
        p.end();
        return;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
  gameLive = false;
}

static void sharksTask(void *) {
  JsonDocument filter;
  JsonObject g = filter["games"].add<JsonObject>();
  g["id"] = true;
  g["gameState"] = true;
  g["startTimeUTC"] = true;
  g["awayTeam"]["abbrev"] = true;
  g["awayTeam"]["score"] = true;
  g["homeTeam"]["abbrev"] = true;
  g["homeTeam"]["score"] = true;
  const String url = "https://api-web.nhle.com/v1/club-schedule/" + String(TEAM) + "/week/now";
  uint32_t seenBeforeStart = 0;   // a game we saw before puck drop: count goals from 0-0

  for (;;) {
    if (!S.sharks || WiFi.status() != WL_CONNECTED) {
      setStatus(S.sharks ? "{\"state\":\"offline\"}" : "{\"state\":\"off\"}");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    JsonDocument doc;
    if (!fetchJson(url, doc, filter)) { vTaskDelay(pdMS_TO_TICKS(30000)); continue; }

    bool followed = false, foundNext = false;
    for (JsonObject game : doc["games"].as<JsonArray>()) {
      const char *state = game["gameState"] | "";
      uint32_t id = game["id"] | 0;
      bool home = !strcmp(game["homeTeam"]["abbrev"] | "", TEAM);
      String opp = game[home ? "awayTeam" : "homeTeam"]["abbrev"] | "???";
      if (isLive(state)) {
        int us = 0, them = 0;
        if (seenBeforeStart != id) {       // joined mid-game: don't replay old goals
          us = game[home ? "homeTeam" : "awayTeam"]["score"] | 0;
          them = game[home ? "awayTeam" : "homeTeam"]["score"] | 0;
        }
        Serial.printf("Sharks game %u live vs %s (%d-%d)\n", id, opp.c_str(), us, them);
        followGame(id, us, them, home, opp);
        followed = true;
        break;
      }
      if (!isOver(state) && !foundNext) {
        foundNext = true;
        seenBeforeStart = id;
        setStatus("{\"state\":\"next\",\"opp\":\"" + opp + "\",\"home\":" + (home ? "true" : "false") +
                  ",\"start\":\"" + String(game["startTimeUTC"] | "") + "\"}");
      }
    }
    if (!followed && !foundNext) setStatus("{\"state\":\"none\"}");
    if (!followed) vTaskDelay(pdMS_TO_TICKS(120000));
  }
}

static void setupSharks() {
  gameEvents = xQueueCreate(6, sizeof(GameEvent));
  gameLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(sharksTask, "sharks", 12288, nullptr, 1, nullptr, 0);
}

// ---- Shark pong (core 1) ----------------------------------------------------
// Teal Sharks paddle on the left, the other team on the right, a white puck.
// During a live game the digits are the real score; otherwise the rally score.
static const char *DIGITS3x5[10] = {
  "111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
  "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111",
};

static void drawDigit(CRGB *buf, uint8_t d, int x0, int y0, CRGB c) {
  for (int i = 0; i < 15; i++)
    if (DIGITS3x5[d % 10][i] == '1') buf[(y0 + i / 3) * W + x0 + i % 3] = c;
}

static void fxPong() {
  static float bx = 7.5f, by = 7.5f, vx = 0.22f, vy = 0.13f, left = 7.5f, right = 7.5f;
  static float aimL = 0, aimR = 0;          // each paddle's miss-prone offset, rerolled per rally
  static uint8_t ptsL = 0, ptsR = 0;
  static uint32_t pauseUntil = 0, chompUntil = 0;
  uint32_t now = millis();
  float k = S.speed / 50.0f;

  auto serve = [&](float dir) {
    bx = 7.5f; by = 4 + random8(8);
    vx = dir * 0.22f; vy = (random8(2) ? 1 : -1) * (0.06f + random8(10) / 100.0f);
    aimL = (random8(100) - 50) / 30.0f;     // up to ~1.7 px off: Sharks usually reach it
    aimR = (random8(100) - 50) / 22.0f;     // up to ~2.3 px off: they miss more
    pauseUntil = now + 400;
  };

  if (now >= pauseUntil) {
    bx += vx * k; by += vy * k;
    if (by < 0) { by = -by; vy = -vy; }
    if (by > H - 1) { by = 2 * (H - 1) - by; vy = -vy; }
    // Paddles chase the puck when it's coming their way, a little imperfectly.
    float tl = vx < 0 ? by + aimL : 7.5f, tr = vx > 0 ? by + aimR : 7.5f;
    left += constrain(tl - left, -0.20f * k, 0.20f * k);
    right += constrain(tr - right, -0.17f * k, 0.17f * k);
    left = constrain(left, 1.5f, H - 2.5f);
    right = constrain(right, 1.5f, H - 2.5f);
    if (bx <= 1 && vx < 0 && fabsf(by - left - 0.5f) <= 2.5f) {
      vx = -vx * 1.04f; vy += (by - left - 0.5f) * 0.04f; bx = 1; chompUntil = now + 150;
    } else if (bx >= W - 2 && vx > 0 && fabsf(by - right - 0.5f) <= 2.5f) {
      vx = -vx * 1.04f; vy += (by - right - 0.5f) * 0.04f; bx = W - 2;
    }
    vx = constrain(vx, -0.45f, 0.45f);
    vy = constrain(vy, -0.3f, 0.3f);
    if (bx < -1) { ptsR++; serve(1); }
    if (bx > W) { ptsL++; serve(-1); }
  }

  fill_solid(frame, NUM_LEDS, CRGB(0, 0, 4));
  for (int y = 0; y < H; y += 2) { px(7, y) = CRGB(0, 25, 30); px(8, y) = CRGB(0, 25, 30); }
  bool live = gameLive;
  CRGB dim(40, 40, 50);
  drawDigit(frame, live ? liveUs : ptsL, 3, 1, TEAL.scale8(110));
  drawDigit(frame, live ? liveThem : ptsR, 10, 1, dim);
  for (int i = -2; i <= 1; i++) {
    int yl = (int)lroundf(left) + i, yr = (int)lroundf(right) + i;
    if (yl >= 0 && yl < H) px(0, yl) = now < chompUntil ? CRGB::White : TEAL;
    if (yr >= 0 && yr < H) px(W - 1, yr) = CRGB(150, 150, 160);
  }
  int x = lroundf(bx), y = lroundf(by);
  if (x >= 0 && x < W && y >= 0 && y < H) px(x, y) = CRGB::White;
}

// ---- Shows (core 1) ---------------------------------------------------------
static GameEvent show{SHOW_NONE, 0, 0, {0}};
static uint32_t showStart = 0;

static String scoreLine(const GameEvent &e) {
  return String(TEAM) + " " + e.us + "  " + e.opp + " " + e.them;
}

// Scrolls `s` once starting at `t0`; returns false when it has gone by.
static bool scrollOnce(CRGB *buf, const String &s, uint32_t t, uint32_t t0, uint8_t scale, uint16_t msPerCol, CRGB c) {
  if (t < t0) return true;
  int total = textWidth(s, scale) + W;
  int off = (t - t0) / msPerCol;
  if (off >= total) return false;
  drawText(buf, s, W - off, scale == 2 ? 1 : 4, scale, c);
  return true;
}

static void teamWaves(CRGB *buf, uint32_t t, uint8_t level) {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint8_t v = sin8(x * 20 + y * 12 + t / 4);
      buf[y * W + x] = TEAL;
      buf[y * W + x].nscale8(scale8(scale8(v, v), level));
    }
}

// The celebration. `words` scrolls after the intro, then the score.
static bool goalShow(CRGB *buf, uint32_t t, const String &words) {
  fill_solid(buf, NUM_LEDS, CRGB::Black);
  if (t < 1500) {                                     // strobe: teal, white, black
    static const CRGB flash[] = {TEAL, CRGB::White, CRGB::Black, ORANGE};
    fill_solid(buf, NUM_LEDS, flash[(t / 90) % 4]);
    return true;
  }
  if (t < 3500) {                                     // rings bursting from the centre
    float r = ((t - 1500) % 1000) / 1000.0f * 12;
    for (int i = 0; i < NUM_LEDS; i++) {
      float d = fabsf(DIST[i] / 16.0f - r);
      if (d < 1.2f) buf[i] = ((t - 1500) / 1000) % 2 ? ORANGE : TEAL;
      else if (random8() < 6) buf[i] = CRGB::White;
    }
    return true;
  }
  if (t < 5500) {                                     // a fin cruising over the waves
    for (int y = 12; y < H; y++)
      for (int x = 0; x < W; x++) {
        uint8_t v = sin8(x * 32 + y * 40 + t / 2);
        if (y > 12 || v > 150) buf[y * W + x] = TEAL;
      }
    int fx = -2 + (int)(t - 3500) * (W + 8) / 2000;   // nose of the fin
    for (int k = 0; k < 6; k++)
      for (int x = fx - 6; x <= fx - k; x++)
        if (x >= 0 && x < W) buf[(11 - k) * W + x] = k == 0 ? CRGB(120, 120, 130) : CRGB(200, 200, 210);
    return true;
  }
  uint32_t textT0 = 5500;
  uint32_t scoreT0 = textT0 + (textWidth(words, 2) + W) * 35;
  teamWaves(buf, t, t < scoreT0 ? 160 : 60);
  if (t < scoreT0) return scrollOnce(buf, words, t, textT0, 2, 35, CRGB::White);
  return scrollOnce(buf, scoreLine(show), t, scoreT0, 1, 55, CRGB::White);
}

// ---- Sad shows: the shark turns to bones, or goes belly-up -------------------------
// 14x7, facing right. '#' body, 'w' belly, 'o' eye.
static const char *SHARK[7] = {
  "........#.....",
  ".......###....",
  "#....#######..",
  "##.#######o##.",
  "##############",
  "##.#wwwwwwww#.",
  "#.....wwwww...",
};
constexpr int SHARK_W = 14, SHARK_H = 7;

// belly-up flips it vertically; deadEye turns the eye red.
static void drawShark(CRGB *buf, int x0, int y0, bool bellyUp, bool deadEye, uint8_t level = 255) {
  for (int r = 0; r < SHARK_H; r++)
    for (int c = 0; c < SHARK_W; c++) {
      char ch = SHARK[bellyUp ? SHARK_H - 1 - r : r][c];
      int x = x0 + c, y = y0 + r;
      if (ch == '.' || x < 0 || x >= W || y < 0 || y >= H) continue;
      CRGB col = ch == '#' ? CRGB(95, 110, 135) : ch == 'w' ? CRGB(200, 200, 210) : deadEye ? CRGB(150, 0, 0) : CRGB(0, 0, 0);
      col.nscale8(level);
      buf[y * W + x] = col;
    }
}

// 14x7 fish skeleton, the same footprint as SHARK so one turns into the
// other in place. '#' bone, 'o' the eye socket (glows red).
static const char *BONES[7] = {
  "#......#......",
  ".#..#.#.#.###.",
  "..#.#.#.#.#o.#",
  "##############",
  "..#.#.#.#.#..#",
  ".#..#.#.#..##.",
  "#.............",
};

static void drawBones(CRGB *buf, int x0, int y0, uint8_t level, uint8_t eye) {
  for (int r = 0; r < SHARK_H; r++)
    for (int c = 0; c < SHARK_W; c++) {
      char ch = BONES[r][c];
      int x = x0 + c, y = y0 + r;
      if (ch == '.' || x < 0 || x >= W || y < 0 || y >= H) continue;
      CRGB col = ch == '#' ? CRGB(225, 220, 195) : CRGB(eye, 0, 0);
      col.nscale8(level);
      buf[y * W + x] = col;
    }
}

static void water(CRGB *buf, uint32_t t, uint8_t level) {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) buf[y * W + x] = CRGB(0, 14 + y * 2, 40 + y * 4).nscale8(level);
  for (int i = 0; i < 4; i++) {                       // bubbles drifting up
    int bx = (i * 5 + 3) % W, by = H - 1 - (int)((t / 90 + i * 11) % H);
    if (by >= 0 && by < H) buf[by * W + bx] = CRGB(50, 100, 150).nscale8(level);
  }
}

// Goal against: the shark swims in, an X-ray flicker, and it's just bones,
// sinking with a glowing red eye.
static bool skeletonShow(CRGB *buf, uint32_t t, const String &words) {
  const int restX = 1, restY = 4;
  if (t < 400) {                                      // a red warning flash
    fill_solid(buf, NUM_LEDS, CRGB(150, 0, 0));
    nscale8(buf, NUM_LEDS, 255 - t * 255 / 400);
    return true;
  }
  if (t < 7000) {
    uint32_t s = t - 400;
    if (s < 1800) {                                   // swims in from the left, bobbing
      water(buf, s, 255);
      int sx = -SHARK_W + (int)s * (restX + SHARK_W) / 1800;
      drawShark(buf, sx, restY + (sin8(s / 3) > 128 ? 0 : 1), false, false);
    } else if (s < 2300) {                            // a beat: it stops
      water(buf, s, 255);
      drawShark(buf, restX, restY, false, false);
    } else if (s < 3300) {                            // X-ray: strobe between shark and bones
      uint32_t u = s - 2300;
      bool bones = (u / 70) % 2;
      if (bones) fill_solid(buf, NUM_LEDS, CRGB(10, 10, 20));
      else water(buf, s, 255);
      if (u < 900 && (u / 70) % 4 == 1) fill_solid(buf, NUM_LEDS, CRGB(200, 200, 220));   // flashes
      if (bones) drawBones(buf, restX, restY, 255, 230);
      else drawShark(buf, restX, restY, false, false);
    } else {                                          // bones alone, sinking and fading
      uint32_t u = s - 3300;
      uint8_t level = u < 2200 ? 255 : 255 - (u - 2200) * 255 / 1400;
      fill_solid(buf, NUM_LEDS, CRGB(4, 4, 12));
      uint8_t eye = 120 + (sin8(u / 3) >> 1);         // the eye socket pulses red
      drawBones(buf, restX, restY + (int)(u / 700), level, scale8(eye, level));
    }
    return true;
  }
  fill_solid(buf, NUM_LEDS, CRGB::Black);
  uint32_t textT0 = 7000;
  uint32_t scoreT0 = textT0 + (textWidth(words, 2) + W) * 55;
  if (t < scoreT0) return scrollOnce(buf, words, t, textT0, 2, 55, CRGB(40, 60, 170));
  return scrollOnce(buf, scoreLine(show), t, scoreT0, 1, 65, CRGB(70, 70, 110));
}

// Loss, kept simple: the shark rolls belly-up and floats away, then a
// tombstone reading RIP rises out of the ground.
static const char *RIP3x5[3] = {"110101110101101", "111010010010111", "110101110100100"};   // R, I, P

static void tombstone(CRGB *buf, int rise, uint8_t level) {
  static const uint8_t STARS[][2] = {{3, 1}, {13, 0}, {0, 9}, {15, 6}, {1, 0}};
  fill_solid(buf, NUM_LEDS, CRGB(4, 4, 18));
  for (auto &st : STARS) buf[st[1] * W + st[0]] = CRGB(90, 90, 110);
  int dy = 14 - rise;                                 // rise 0..14 lifts it out of the ground
  for (int y = 0; y < 14; y++)
    for (int x = 0; x < W; x++) {
      int sy = y - dy;                                // row within the stone
      bool stone = (sy == 1 && x >= 4 && x <= 11) || (sy == 2 && x >= 2 && x <= 13) || (sy >= 3 && x >= 1 && x <= 14);
      if (stone) buf[y * W + x] = CRGB(115, 115, 125);
    }
  for (int n = 0; n < 3; n++)
    for (int i = 0; i < 15; i++) {
      int y = 4 + i / 3 + dy, x = 3 + n * 4 + i % 3;
      if (RIP3x5[n][i] == '1' && y >= 0 && y < 14) buf[y * W + x] = CRGB(25, 25, 35);
    }
  for (int y = 14; y < H; y++)
    for (int x = 0; x < W; x++) buf[y * W + x] = (x + y) % 3 ? CRGB(20, 45, 15) : CRGB(30, 60, 20);
  nscale8(buf, NUM_LEDS, level);
}

static bool lossShow(CRGB *buf, uint32_t t, const String &words) {
  const CRGB SEA(0, 15, 45);
  const int restX = 1, restY = 4;
  if (t < 4000) {
    fill_solid(buf, NUM_LEDS, SEA);
    if (t < 1500) {                                   // swims in
      drawShark(buf, -SHARK_W + (int)t * (restX + SHARK_W) / 1500, restY, false, false);
    } else if (t < 2400) {                            // rolls: over, back, over for good
      bool up = (t - 1500) / 300 != 1;
      drawShark(buf, restX, restY, up, up);
    } else {                                          // floats up and away, belly-up
      uint32_t u = t - 2400;
      drawShark(buf, restX, restY - (int)(u * (restY + SHARK_H) / 1600), true, true);
    }
    return true;
  }
  if (t < 8000) {
    uint32_t u = t - 4000;
    int rise = u < 1200 ? (int)(u * 14 / 1200) : 14;
    uint8_t level = u < 3000 ? 255 : 255 - (u - 3000) * 180 / 1000;
    tombstone(buf, rise, level);
    return true;
  }
  fill_solid(buf, NUM_LEDS, CRGB::Black);
  uint32_t textT0 = 8000;
  uint32_t scoreT0 = textT0 + (textWidth(words, 2) + W) * 60;
  if (t < scoreT0) return scrollOnce(buf, words, t, textT0, 2, 60, CRGB(110, 110, 125));
  return scrollOnce(buf, scoreLine(show), t, scoreT0, 1, 65, CRGB(80, 80, 95));
}

// ---- The win: fin, JAWS, fireworks ------------------------------------------
// Full-screen jaws with big triangular fangs (5 wide, 3 tall, every 6 px).
// Closed, the top fangs fill rows 5-7 and the bottom ones rows 8-10, meshing
// between each other. `gap` opens them; `shake` jolts them sideways.
static void drawJaws(CRGB *buf, int gap, int shake) {
  const CRGB SKIN(85, 100, 125), GUM(170, 30, 60), TOOTH(240, 240, 235), THROAT(25, 0, 8);
  int top = 5 - gap / 2;              // first row of the top fangs
  int bot = 10 + (gap - gap / 2);     // last row of the bottom fangs
  auto fang = [](int x, int k) { int m = x % 6; return m >= k && m <= 4 - k; };   // row k from the base
  for (int y = 0; y < H; y++)
    for (int xx = 0; xx < W; xx++) {
      int x = xx + shake + 60;        // keep the modulo positive
      CRGB c = THROAT;
      if (y < top - 2) c = SKIN;
      else if (y < top) c = GUM;
      else if (y <= top + 2 && fang(x, y - top)) c = TOOTH;
      if (y > bot + 2) c = SKIN;
      else if (y > bot) c = GUM;
      else if (y >= bot - 2 && fang(x + 3, bot - y)) c = TOOTH;
      buf[y * W + xx] = c;
    }
}

// Fireworks: a new burst every 300 ms, twelve sparks each, drooping as they fade.
static void fireworks(CRGB *buf, uint32_t t, uint8_t level) {
  static const CRGB COLORS[] = {TEAL, ORANGE, CRGB::White, CRGB(0, 220, 200)};
  for (int b = max(0, (int)(t / 300) - 3); b <= (int)(t / 300); b++) {
    uint32_t age = t - b * 300;
    if (age > 1000) continue;
    uint32_t h = b * 2654435761u;
    float cx = 3 + h % 10, cy = 2 + (h >> 8) % 8;
    float r = age / 110.0f, droop = (age / 400.0f) * (age / 400.0f);
    uint8_t fade = 255 - age * 255 / 1000;
    CRGB c = COLORS[b % 4];
    c.nscale8(scale8(fade, level));
    for (int k = 0; k < 12; k++) {
      float a = k * (TWO_PI / 12) + (h % 7) * 0.1f;
      int x = lroundf(cx + cosf(a) * r), y = lroundf(cy + sinf(a) * r + droop);
      if (x >= 0 && x < W && y >= 0 && y < H) buf[y * W + x] += c;
    }
    if (age < 120) buf[(int)cy * W + (int)cx] = CRGB::White;
  }
}

static bool winShow(CRGB *buf, uint32_t t) {
  fill_solid(buf, NUM_LEDS, CRGB::Black);
  if (t < 3200) {
    // dun-dun... dun-dun, quickening, while a fin crosses faster and faster
    static const uint16_t BEATS[] = {0, 650, 1200, 1650, 2000, 2300, 2550, 2750, 2920, 3060};
    uint8_t pulse = 0;
    for (uint16_t b : BEATS)
      if (t >= b && t < b + 220u) pulse = max<uint8_t>(pulse, 255 - (t - b) * 255 / 220);
    fill_solid(buf, NUM_LEDS, CRGB(90, 0, 10).scale8(pulse));
    for (int y = 12; y < H; y++)
      for (int x = 0; x < W; x++) {
        uint8_t v = sin8(x * 32 + y * 40 + t / 3);
        if (y > 12 || v > 150) buf[y * W + x] = CRGB(0, 70, 85);
      }
    float p = t / 3200.0f;
    int fx = -2 + (int)(p * p * (W + 9));             // accelerating
    for (int k = 0; k < 6; k++)
      for (int x = fx - 6; x <= fx - k; x++)
        if (x >= 0 && x < W) buf[(11 - k) * W + x] = k == 0 ? CRGB(120, 120, 130) : CRGB(200, 200, 210);
    return true;
  }
  if (t < 6000) {
    uint32_t s = t - 3200;
    // gap over time: slam in and shut in one stroke, open, chomp, open wide, CHOMP
    struct Key { uint16_t at; int8_t gap; };
    static const Key KEYS[] = {
      {0, 22}, {320, 0}, {650, 0}, {950, 9}, {1100, 0}, {1400, 0}, {1750, 12},
      {1880, 0}, {2800, 0},
    };
    int gap = 0;
    for (size_t i = 0; i + 1 < sizeof(KEYS) / sizeof(KEYS[0]); i++)
      if (s >= KEYS[i].at && s < KEYS[i + 1].at) {
        // Bites speed up as they close (p squared); openings ease out.
        float p = (float)(s - KEYS[i].at) / (KEYS[i + 1].at - KEYS[i].at);
        bool closing = KEYS[i + 1].gap < KEYS[i].gap;
        float e = closing ? p * p : 1 - (1 - p) * (1 - p);
        gap = KEYS[i].gap + (int)lroundf(e * (KEYS[i + 1].gap - KEYS[i].gap));
        break;
      }
    bool shut = gap == 0;
    int shake = shut ? ((s / 40) % 2 ? 1 : -1) : 0;
    drawJaws(buf, gap, shake);
    if (s >= 1880) {                                  // the last chomp: a white flash
      uint8_t f = s < 2330 ? 255 - (s - 1880) * 255 / 450 : 0;
      for (int i = 0; i < NUM_LEDS; i++) buf[i] = blend(buf[i], CRGB::White, f);
    }
    return true;
  }
  uint32_t s = t - 6000;
  if (s < 3000) { fireworks(buf, s, 255); return true; }
  uint32_t textT0 = 3000;
  const String words = "SHARKS WIN!";
  uint32_t scoreT0 = textT0 + (textWidth(words, 2) + W) * 35;
  teamWaves(buf, t, s < scoreT0 ? 140 : 60);
  fireworks(buf, s, 90);
  if (s < scoreT0) return scrollOnce(buf, words, s, textT0, 2, 35, CRGB::White);
  return scrollOnce(buf, scoreLine(show), s, scoreT0, 1, 55, CRGB::White);
}

// Draws the current show into buf. Returns false when nothing is playing.
static bool renderShow(CRGB *buf, uint32_t now) {
  if (show.kind == SHOW_NONE) {
    GameEvent next;
    if (!xQueueReceive(gameEvents, &next, 0)) return false;
    show = next;
    showStart = now;
  }
  uint32_t t = now - showStart;
  bool playing;
  switch (show.kind) {
    case SHOW_GOAL:    playing = goalShow(buf, t, "GOAL!"); break;
    case SHOW_WIN:     playing = winShow(buf, t); break;
    case SHOW_AGAINST: playing = skeletonShow(buf, t, "UGH."); break;
    default:           playing = lossShow(buf, t, "FINAL"); break;
  }
  if (!playing) show.kind = SHOW_NONE;
  return playing;
}
