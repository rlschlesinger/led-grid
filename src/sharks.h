// Sharks goal alerts. Included once, from main.cpp, after the drawing helpers.
//
// A task on core 0 polls api-web.nhle.com: the Sharks' week schedule every
// 2 minutes, and the live game every 10 seconds. Score changes become events
// on a queue. The render loop on core 1 plays each as a full-screen show over
// whatever effect is running:
//   Sharks goal   teal/white strobe, rings, a fin swimming, "GOAL!", the score
//   goal against  red flash, a crying face in the rain, "UGH", the score
//   final         the goal show with "SHARKS WIN!", or the sad one with "FINAL"
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

// Follows one live game until it ends. Returns when it's over or Wi-Fi drops.
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

  while (S.sharks && WiFi.status() == WL_CONNECTED) {
    JsonDocument doc;
    if (fetchJson(url, doc, filter)) {
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
  static uint32_t pauseUntil = 0, chompUntil = 0, flashUntil = 0;
  static CRGB flashColor;
  uint32_t now = millis();
  float k = S.speed / 50.0f;

  auto serve = [&](float dir) {
    bx = 7.5f; by = 4 + random8(8);
    vx = dir * 0.22f; vy = (random8(2) ? 1 : -1) * (0.06f + random8(10) / 100.0f);
    aimL = (random8(100) - 50) / 30.0f;     // up to ~1.7 px off: Sharks usually reach it
    aimR = (random8(100) - 50) / 22.0f;     // up to ~2.3 px off: they miss more
    pauseUntil = now + 600;
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
    if (bx < -1) { ptsR++; flashColor = CRGB(60, 0, 0); flashUntil = now + 300; serve(1); }
    if (bx > W) { ptsL++; flashColor = TEAL; flashUntil = now + 300; serve(-1); }
  }

  fill_solid(frame, NUM_LEDS, now < flashUntil ? flashColor.scale8(80) : CRGB(0, 0, 4));
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

static const char *SAD_FACE[H] = {
  ".....######.....",
  "...##......##...",
  "..#..........#..",
  ".#............#.",
  ".#............#.",
  "#...##....##...#",
  "#...##....##...#",
  "#..............#",
  "#..............#",
  "#.....####.....#",
  "#....#....#....#",
  ".#..#......#..#.",
  ".#............#.",
  "..#..........#..",
  "...##......##...",
  ".....######.....",
};

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

// The sad one: a red flash, then a crying face in the rain, then words.
static bool sadShow(CRGB *buf, uint32_t t, const String &words) {
  fill_solid(buf, NUM_LEDS, CRGB::Black);
  if (t < 700) {
    fill_solid(buf, NUM_LEDS, CRGB(160, 0, 0));
    nscale8(buf, NUM_LEDS, 255 - t * 255 / 700);
    return true;
  }
  if (t < 5200) {
    uint32_t s = t - 700;
    for (int x = 0; x < W; x++) {                     // rain: one drop per column
      int y = ((s / 70) + x * 7 + (x * x) % 5) % (H + 6);
      if (y < H) buf[y * W + x] = CRGB(0, 10, 45);
    }
    uint8_t fade = s < 3500 ? 255 : 255 - (s - 3500) * 255 / 1000;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        if (SAD_FACE[y][x] == '#') { buf[y * W + x] = CRGB(40, 70, 200); buf[y * W + x].nscale8(fade); }
    int tear = 7 + (s / 160) % 7;                     // a tear runs from the left eye
    buf[tear * W + 4] = CRGB(90, 160, 255);
    buf[tear * W + 4].nscale8(fade);
    return true;
  }
  uint32_t textT0 = 5200;
  uint32_t scoreT0 = textT0 + (textWidth(words, 2) + W) * 60;
  if (t < scoreT0) return scrollOnce(buf, words, t, textT0, 2, 60, CRGB(40, 60, 170));
  return scrollOnce(buf, scoreLine(show), t, scoreT0, 1, 65, CRGB(70, 70, 110));
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
    case SHOW_WIN:     playing = goalShow(buf, t, "SHARKS WIN!"); break;
    case SHOW_AGAINST: playing = sadShow(buf, t, "UGH."); break;
    default:           playing = sadShow(buf, t, "FINAL"); break;
  }
  if (!playing) show.kind = SHOW_NONE;
  return playing;
}
