// Knicks alerts. Included once, from main.cpp, after sharks.h (it shares the
// show queue, fetch helper and drawing helpers there).
//
// Data: ESPN's public API, no key. A task on core 0 checks the Knicks' team
// page (nextEvent: the next or current game) every 2 minutes; while a game is
// live it polls each team's score (~256 B each) every 6 s and the clock every
// 18 s from ESPN's core API.
//
// Basketball scores ~45 times a game per side, so the shows are tiered:
//   Knicks basket   a shot arcs in, swish, "+2" / "+1"        (2.5 s)
//   Knicks three    the same, then a big "3" in a burst        (3.5 s)
//   opponent basket the screen dims with a red edge flicker    (0.9 s)
//   Knicks run 8+   NBA Jam: a basketball on fire, "8-0 RUN"
//   Knicks lead     blue/orange stripes, "KNICKS LEAD"
//   opponent run 8+ / takes the lead: a brick off the rim, "0-8 RUN" / "LEAD LOST"
//   win / loss      the NYC skyline: fireworks and "KNICKS WIN!", or the
//                   lights going out in the rain and "FINAL"
// Big shows (runs, leads) have a 90 s cooldown between them.
#pragma once

static const char *KNICKS = "NYK";
static const char *KNICKS_ESPN = "ny";       // ESPN's team slug
static const CRGB KBLUE(0, 85, 200), KORANGE(245, 110, 20), KSILVER(150, 155, 165);
static const CRGB COURT_BG(0, 4, 22);

static volatile uint8_t knUs = 0, knThem = 0;
static String knicksStatus = "{\"state\":\"idle\"}";   // guarded by gameLock

static void setKnicksStatus(const String &s) {
  xSemaphoreTake(gameLock, portMAX_DELAY);
  knicksStatus = s;
  xSemaphoreGive(gameLock);
}

static String currentKnicksStatus() {
  xSemaphoreTake(gameLock, portMAX_DELAY);
  String s = knicksStatus;
  xSemaphoreGive(gameLock);
  return s;
}

// ---- Fetching (core 0) ------------------------------------------------------
// One kept-alive HTTPS connection for the tiny core-API requests, so a live
// game costs one TLS handshake rather than one every few seconds.
static bool fetchCore(const String &url, JsonDocument &doc) {
  static WiFiClientSecure client;
  static HTTPClient http;
  static bool init = false;
  if (!init) { client.setInsecure(); http.setReuse(true); http.setTimeout(4000); init = true; }
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = code == 200 && !deserializeJson(doc, http.getString(), DeserializationOption::NestingLimit(32));
  if (code != 200) { Serial.printf("ESPN %s -> HTTP %d\n", url.c_str(), code); http.end(); client.stop(); }
  return ok;
}

static void followKnicks(const String &eid, const String &usId, const String &themId, const String &opp) {
  const String base = "https://sports.core.api.espn.com/v2/sports/basketball/leagues/nba/events/" + eid +
                      "/competitions/" + eid;
  int us = -1, them = -1, lead = 0, usRun = 0, themRun = 0;
  bool usRunShown = false, themRunShown = false;
  uint32_t lastBig = 0, lastStatus = 0;
  String period = "Q1", clock = "";
  Preferences p;

  while (S.knicks) {
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
    JsonDocument a, b;
    if (!fetchCore(base + "/competitors/" + usId + "/score", a) || !fetchCore(base + "/competitors/" + themId + "/score", b)) {
      vTaskDelay(pdMS_TO_TICKS(6000));
      continue;
    }
    int nowUs = a["value"] | 0, nowThem = b["value"] | 0;
    if (us < 0) { us = nowUs; them = nowThem; lead = us - them; }   // joined mid-game: no replays

    bool bigOk = millis() - lastBig > 90000;
    int dUs = nowUs - us, dThem = nowThem - them;
    if (dUs > 0) {
      pushEvent(dUs == 3 ? K_THREE : K_SWISH, nowUs, nowThem, opp.c_str(), dUs);
      usRun += dUs; themRun = 0; themRunShown = false;
      if (usRun >= 8 && !usRunShown && bigOk) { pushEvent(K_RUN, nowUs, nowThem, opp.c_str(), usRun); usRunShown = true; lastBig = millis(); bigOk = false; }
    }
    if (dThem > 0) {
      pushEvent(K_OPP, nowUs, nowThem, opp.c_str(), dThem);
      themRun += dThem; usRun = 0; usRunShown = false;
      if (themRun >= 8 && !themRunShown && bigOk) { pushEvent(K_BRICK, nowUs, nowThem, opp.c_str(), themRun); themRunShown = true; lastBig = millis(); bigOk = false; }
    }
    int nowLead = nowUs - nowThem;
    if (bigOk && lead <= 0 && nowLead > 0) { pushEvent(K_LEAD, nowUs, nowThem, opp.c_str()); lastBig = millis(); }
    else if (bigOk && lead >= 0 && nowLead < 0) { pushEvent(K_BRICK, nowUs, nowThem, opp.c_str(), 0); lastBig = millis(); }
    lead = nowLead;
    us = nowUs; them = nowThem;
    knUs = us; knThem = them;

    bool over = false;
    if (millis() - lastStatus > 18000 || lastStatus == 0) {
      JsonDocument st;
      if (fetchCore(base + "/status", st)) {
        lastStatus = millis();
        const char *state = st["type"]["state"] | "in";
        over = !strcmp(state, "post");
        int q = st["period"] | 1;
        period = q <= 4 ? "Q" + String(q) : "OT" + String(q - 4 > 1 ? String(q - 4) : "");
        clock = st["type"]["name"] == "STATUS_HALFTIME" ? "HALF" : String(st["displayClock"] | "");
      }
    }
    knicksLive = !over;
    setKnicksStatus("{\"state\":\"" + String(over ? "final" : "live") + "\",\"us\":" + us + ",\"them\":" + them +
                    ",\"opp\":\"" + opp + "\",\"period\":\"" + period + "\",\"clock\":\"" + clock + "\"}");
    if (over) {
      p.begin("knicks", false);
      if (p.getString("done", "") != eid) {
        p.putString("done", eid);
        pushEvent(us > them ? K_WIN : K_LOSS, us, them, opp.c_str());
      }
      p.end();
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(6000));
  }
  knicksLive = false;
}

static void knicksTask(void *) {
  JsonDocument filter;
  JsonObject ev = filter["team"]["nextEvent"].add<JsonObject>();
  ev["id"] = true;
  ev["date"] = true;
  JsonObject comp = ev["competitions"].add<JsonObject>();
  comp["status"]["type"]["state"] = true;
  JsonObject who = comp["competitors"].add<JsonObject>();
  who["id"] = true;
  who["homeAway"] = true;
  who["team"]["abbreviation"] = true;
  const String url = "https://site.api.espn.com/apis/site/v2/sports/basketball/nba/teams/" + String(KNICKS_ESPN);

  for (;;) {
    if (!S.knicks || WiFi.status() != WL_CONNECTED) {
      setKnicksStatus(S.knicks ? "{\"state\":\"offline\"}" : "{\"state\":\"off\"}");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    JsonDocument doc;
    if (!fetchJson(url, doc, filter)) { vTaskDelay(pdMS_TO_TICKS(30000)); continue; }
    JsonObject e = doc["team"]["nextEvent"][0];
    if (e.isNull()) { setKnicksStatus("{\"state\":\"none\"}"); vTaskDelay(pdMS_TO_TICKS(600000)); continue; }

    String eid = e["id"] | "";
    const char *state = e["competitions"][0]["status"]["type"]["state"] | "pre";
    String usId, themId, opp = "???";
    bool home = false;
    for (JsonObject c : e["competitions"][0]["competitors"].as<JsonArray>()) {
      String abbr = c["team"]["abbreviation"] | "";
      if (abbr == "NY") { usId = c["id"] | ""; home = !strcmp(c["homeAway"] | "", "home"); }
      else { themId = c["id"] | ""; opp = abbr; }
    }
    if (!strcmp(state, "in") && usId.length() && themId.length()) {
      Serial.printf("Knicks game %s live vs %s\n", eid.c_str(), opp.c_str());
      followKnicks(eid, usId, themId, opp);
      continue;
    }
    setKnicksStatus("{\"state\":\"next\",\"opp\":\"" + opp + "\",\"home\":" + (home ? "true" : "false") +
                    ",\"start\":\"" + String(e["date"] | "") + "\"}");
    vTaskDelay(pdMS_TO_TICKS(120000));
  }
}

static void setupKnicks() {
  xTaskCreatePinnedToCore(knicksTask, "knicks", 12288, nullptr, 0, nullptr, 0);   // idle priority: see setupSharks()
}

// ---- Drawing ----------------------------------------------------------------
static void ball(CRGB *buf, int x0, int y0, int size = 4) {          // a basketball, 4x4 or 6x6
  for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++) {
      bool corner = (x == 0 || x == size - 1) && (y == 0 || y == size - 1);
      int px_ = x0 + x, py = y0 + y;
      if (corner || px_ < 0 || px_ >= W || py < 0 || py >= H) continue;
      bool seam = x == size / 2 - (size > 4 ? 0 : 1) || y == size / 2;
      buf[py * W + px_] = seam ? CRGB(90, 30, 0) : KORANGE;
    }
}

// Hoop on the right: backboard at x=15, rim on row 5, net hanging below.
static void hoop(CRGB *buf, uint8_t netGlow = 0) {
  for (int y = 1; y <= 7; y++) buf[y * W + 15] = CRGB(200, 200, 210);
  for (int x = 10; x <= 14; x++) buf[5 * W + x] = CRGB(255, 80, 0);
  CRGB net = blend(CRGB(110, 110, 120), CRGB::White, netGlow);
  for (int y = 6; y <= 8; y++)
    for (int x = 11; x <= 13; x++)
      if ((x + y) % 2 == 0) buf[y * W + x] = net;
}

static void plusPoints(CRGB *buf, int pts, CRGB c) {   // "+2" style, in 3x5 digits at the left
  int x = 1, y = 9;
  buf[(y + 2) * W + x] = c; buf[(y + 2) * W + x + 2] = c;
  for (int k = 0; k < 3; k++) buf[(y + 1 + k) * W + x + 1] = c;
  drawDigit(buf, pts, x + 4, y, c);
}

// A shot from the bottom-left, arcing in over the rim and dropping through.
// Returns the time it took. `made` false = it hits the rim and bounces off.
static void shot(CRGB *buf, uint32_t t, bool made) {
  hoop(buf, made && t > 1000 && t < 1500 ? 255 - (t - 1000) * 255 / 500 : 0);
  if (t < 1000) {                                    // the arc
    float p = t / 1000.0f;
    ball(buf, (int)(0 + 10 * p), (int)(12 - 20 * p + 10 * p * p));
  } else if (made && t < 1500) {                     // through the net
    ball(buf, 10, 2 + (int)((t - 1000) * 12 / 500));
  } else if (!made && t < 2200) {                    // clank: up and away to the left
    float q = (t - 1000) / 1200.0f;
    ball(buf, (int)(10 - 12 * q), (int)(1 - 6 * q + 18 * q * q));
    if (t < 1150) for (int x = 10; x <= 14; x++) buf[5 * W + x] = CRGB::White;   // the rim rings
  }
}

static void kFireworks(CRGB *buf, uint32_t t, uint8_t level) {
  static const CRGB COLORS[] = {KBLUE, KORANGE, CRGB::White, CRGB(60, 140, 255)};
  fireworks(buf, t, level, COLORS);
}

static String kScore(const GameEvent &e) {
  return String(KNICKS) + " " + e.us + "  " + e.opp + " " + e.them;
}

// NYC skyline: building tops per column (lower = taller); the Empire State
// Building is columns 7-8 with its crown on rows 3-5 and a spire above.
static const uint8_t SKYLINE[W] = {10, 10, 12, 8, 8, 11, 7, 3, 3, 7, 11, 9, 9, 12, 7, 7};

static void skyline(CRGB *buf, uint32_t t, uint8_t lightsOn, bool crown) {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) buf[y * W + x] = blend(CRGB(0, 0, 12), CRGB(8, 5, 30), y * 255 / H);
  for (int x = 0; x < W; x++)
    for (int y = SKYLINE[x]; y < H; y++) {
      buf[y * W + x] = CRGB(38, 44, 66);   // buildings, a shade lighter than the sky
      uint32_t h = (x * 31 + y * 17) * 2654435761u;
      if ((h >> 24) < lightsOn && x % 2 == 0 && y % 2 && y > SKYLINE[x]) buf[y * W + x] = CRGB(150, 120, 40);   // windows
    }
  buf[1 * W + 7] = buf[2 * W + 7] = buf[1 * W + 8] = buf[2 * W + 8] = CRGB(60, 60, 70);   // spire
  if (crown) {                                       // the ESB crown chases blue and orange
    for (int y = 3; y <= 5; y++)
      for (int x = 7; x <= 8; x++) buf[y * W + x] = ((y + x + t / 200) % 2) ? KBLUE : KORANGE;
  }
}

static bool knicksWin(CRGB *buf, uint32_t t) {
  if (t < 3000) { skyline(buf, t, 140, true); return true; }
  if (t < 7000) { skyline(buf, t, 140, true); kFireworks(buf, t - 3000, 255); return true; }
  uint32_t s = t - 7000;
  const String words = "KNICKS WIN!";
  uint32_t scoreT0 = (textWidth(words, 2) + W) * 35;
  for (int y = 0; y < H; y++)                        // blue and orange waves
    for (int x = 0; x < W; x++) {
      uint8_t v = sin8(x * 20 + y * 12 + t / 4);
      buf[y * W + x] = blend(KBLUE, KORANGE, v).nscale8(s < scoreT0 ? 120 : 50);
    }
  kFireworks(buf, s, 70);
  if (s < scoreT0) return scrollOnce(buf, words, s, 0, 2, 35, CRGB::White);
  return scrollOnce(buf, kScore(show), s, scoreT0, 1, 55, CRGB::White);
}

static bool knicksLoss(CRGB *buf, uint32_t t) {
  if (t < 7000) {
    uint8_t lights = t < 1000 ? 140 : t < 5000 ? 140 - (t - 1000) * 140 / 4000 : 0;   // windows go out
    skyline(buf, t, lights, t < 4000);
    for (int x = 0; x < W; x++) {                    // rain
      int y = ((t / 60) + x * 5 + (x * x) % 7) % (H + 4);
      if (y < H) buf[y * W + x] += CRGB(20, 30, 70);
    }
    if (t > 5500) nscale8(buf, NUM_LEDS, 255 - (t - 5500) * 200 / 1500);
    return true;
  }
  fill_solid(buf, NUM_LEDS, CRGB::Black);
  uint32_t s = t - 7000, scoreT0 = (textWidth("FINAL", 2) + W) * 60;
  if (s < scoreT0) return scrollOnce(buf, "FINAL", s, 0, 2, 60, KSILVER);
  return scrollOnce(buf, kScore(show), s, scoreT0, 1, 65, CRGB(90, 95, 110));
}

// NBA Jam: a basketball on fire, then the run.
static bool knicksRun(CRGB *buf, uint32_t t) {
  static uint8_t heat[W][H];
  if (t < 3200) {
    for (int x = 0; x < W; x++) {
      for (int y = 0; y < H; y++) heat[x][y] = qsub8(heat[x][y], random8(0, 30));
      for (int y = H - 1; y >= 2; y--) heat[x][y] = (heat[x][y - 1] + heat[x][y - 2] * 2) / 3;
      if (x >= 4 && x <= 11 && random8() < 200) heat[x][random8(4, 7)] = qadd8(heat[x][4], random8(170, 255));
    }
    for (int x = 0; x < W; x++)
      for (int y = 0; y < H; y++) buf[(H - 1 - y) * W + x] = ColorFromPalette(HeatColors_p, scale8(heat[x][y], 230));
    int bounce = abs((int)((t / 30) % 8) - 4) / 2;   // the ball bobs in the flames
    ball(buf, 5, 9 - bounce, 6);
    return true;
  }
  memset(heat, 0, sizeof(heat));
  uint32_t s = t - 3200;
  String words = String(show.extra) + "-0 RUN";
  uint32_t textEnd = (textWidth("ON FIRE", 1) + W) * 45, runEnd = textEnd + (textWidth(words, 2) + W) * 40;
  for (int i = 0; i < NUM_LEDS; i++) buf[i] = KBLUE.scale8(70 + scale8(sin8(i * 9 + t / 3), 50));
  if (s < textEnd) return scrollOnce(buf, "ON FIRE", s, 0, 1, 45, KORANGE);
  return scrollOnce(buf, words, s, textEnd, 2, 40, CRGB::White) || false;
}

static bool knicksLead(CRGB *buf, uint32_t t) {
  for (int y = 0; y < H; y++)                        // stripes marching diagonally
    for (int x = 0; x < W; x++) buf[y * W + x] = ((x + y + t / 60) / 3) % 2 ? KBLUE : KORANGE;
  if (t < 1400) return true;
  nscale8(buf, NUM_LEDS, 60);
  return scrollOnce(buf, "KNICKS LEAD", t - 1400, 0, 1, 45, CRGB::White);
}

// Opponent run (extra = run length) or the opponent taking the lead (extra 0).
static bool knicksBrick(CRGB *buf, uint32_t t) {
  fill_solid(buf, NUM_LEDS, COURT_BG);
  if (t < 2400) { shot(buf, t, false); return true; }
  String words = show.extra ? "0-" + String(show.extra) + " RUN" : String("LEAD LOST");
  return scrollOnce(buf, words, t - 2400, 0, 1, 55, KSILVER);
}

static bool knicksSwish(CRGB *buf, uint32_t t, bool three) {
  fill_solid(buf, NUM_LEDS, COURT_BG);
  if (t < 1500) { shot(buf, t, true); return true; }
  uint32_t s = t - 1500;
  if (!three) {
    if (s > 1000) return false;
    uint8_t pulse = 255 - s * 255 / 1000;
    for (int i = 0; i < NUM_LEDS; i++) buf[i] = KBLUE.scale8(scale8(pulse, 120));
    hoop(buf, pulse);
    plusPoints(buf, show.extra ? show.extra : 2, KORANGE);
    return true;
  }
  if (s > 2000) return false;                        // three: a burst and a big 3
  float r = s / 2000.0f * 12;
  for (int i = 0; i < NUM_LEDS; i++) {
    float d = fabsf(DIST[i] / 16.0f - r);
    buf[i] = d < 1.5f ? ((s / 250) % 2 ? KORANGE : KBLUE) : CRGB::Black;
  }
  drawText(buf, "3", 3, 1, 2, CRGB::White);
  return true;
}

// Opponent basket: the current animation dims, with a red edge flicker.
static bool knicksOpp(CRGB *buf, uint32_t t) {
  if (t > 900) return false;
  memcpy(buf, frame, sizeof(CRGB) * NUM_LEDS);
  nscale8(buf, NUM_LEDS, 90);
  if ((t / 150) % 2 == 0)
    for (int i = 0; i < W; i++) {
      buf[i] = buf[(H - 1) * W + i] = CRGB(140, 0, 0);
      buf[i * W] = buf[i * W + W - 1] = CRGB(140, 0, 0);
    }
  return true;
}

static bool knicksShow(CRGB *buf, uint32_t t, const GameEvent &e) {
  switch (e.kind) {
    case K_SWISH: return knicksSwish(buf, t, false);
    case K_THREE: return knicksSwish(buf, t, true);
    case K_OPP:   return knicksOpp(buf, t);
    case K_RUN:   return knicksRun(buf, t);
    case K_LEAD:  return knicksLead(buf, t);
    case K_BRICK: return knicksBrick(buf, t);
    case K_WIN:   return knicksWin(buf, t);
    default:      return knicksLoss(buf, t);
  }
}

// ---- Knicks court: the game-time screen -------------------------------------
// The live score (Knicks orange on top, the opponent grey below) with a ball
// dribbling across between them.
static void drawScoreRow(CRGB *buf, int score, int y, CRGB c) {
  String s = String(score);
  drawDigits(buf, s, (W - digitsWidth(s)) / 2, y, c);
}

static void fxCourt() {
  fill_solid(frame, NUM_LEDS, COURT_BG);
  for (int x = 0; x < W; x++) frame[8 * W + x] = CRGB(10, 14, 40);   // half-court line
  drawScoreRow(frame, knUs, 1, KORANGE);
  drawScoreRow(frame, knThem, 10, KSILVER);
  frame[3 * W + 0] = KBLUE; frame[12 * W + 0] = CRGB(60, 60, 70);     // team markers
  uint32_t t = millis();
  int x = (int)((t / 90) % 26);
  x = x < 13 ? x : 26 - x;                                            // back and forth
  int y = 6 + abs((int)((t / 70) % 4) - 2);                           // dribble: rows 6..9
  frame[y * W + x + 1] = frame[y * W + x + 2] = KORANGE;
  if (y + 1 < H) frame[(y + 1) * W + x + 1] = frame[(y + 1) * W + x + 2] = CRGB(180, 70, 10);
}
