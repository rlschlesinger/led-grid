// Basketball shows, and the scoreboard game screen for basketball, football
// and baseball. Included once, from main.cpp, after sharks.h (it shares the
// show queue and drawing helpers there).
//
// Basketball scores ~45 times a game per side, so its shows are tiered
// (games.h decides which), all in the team's colours:
//   basket          a shot arcs in, swish, "+2" / "+1"        (2.5 s)
//   three           the same, then a big "3" in a burst        (3.5 s)
//   opponent basket the screen dims with a red edge flicker    (0.9 s)
//   run 8+          NBA Jam: a basketball on fire, "8-0 RUN"
//   lead            stripes in the team colours, "XXX LEAD"
//   opponent run 8+ / takes the lead: a brick off the rim, "0-8 RUN" / "LEAD LOST"
//   win / loss      fireworks and "XXX WIN!", or the tombstone. The Knicks get
//                   the NYC skyline: fireworks over it, or its lights going out
//                   in the rain.
#pragma once

static const CRGB KSILVER(150, 155, 165);
static const CRGB BALL(245, 110, 20);        // a basketball is orange whoever's playing
static const CRGB COURT_BG(0, 4, 22);

// ---- Drawing ----------------------------------------------------------------
static void ball(CRGB *buf, int x0, int y0, int size = 4) {          // a basketball, 4x4 or 6x6
  for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++) {
      bool corner = (x == 0 || x == size - 1) && (y == 0 || y == size - 1);
      int px_ = x0 + x, py = y0 + y;
      if (corner || px_ < 0 || px_ >= W || py < 0 || py >= H) continue;
      bool seam = x == size / 2 - (size > 4 ? 0 : 1) || y == size / 2;
      buf[py * W + px_] = seam ? CRGB(90, 30, 0) : BALL;
    }
}

// Hoop on the right: backboard on the last column, rim on row 5, net hanging below.
constexpr int RIM = W - 6;            // the rim's left end (10 on one panel)
static void hoop(CRGB *buf, uint8_t netGlow = 0) {
  for (int y = 1; y <= 7; y++) buf[y * W + W - 1] = CRGB(200, 200, 210);
  for (int x = RIM; x <= RIM + 4; x++) buf[5 * W + x] = CRGB(255, 80, 0);
  CRGB net = blend(CRGB(110, 110, 120), CRGB::White, netGlow);
  for (int y = 6; y <= 8; y++)
    for (int x = RIM + 1; x <= RIM + 3; x++)
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
    ball(buf, (int)(0 + RIM * p), (int)(12 - 20 * p + 10 * p * p));
  } else if (made && t < 1500) {                     // through the net
    ball(buf, RIM, 2 + (int)((t - 1000) * 12 / 500));
  } else if (!made && t < 2200) {                    // clank: up and away to the left
    float q = (t - 1000) / 1200.0f;
    ball(buf, (int)(RIM - 12 * PANELS * q), (int)(1 - 6 * q + 18 * q * q));
    if (t < 1150) for (int x = RIM; x <= RIM + 4; x++) buf[5 * W + x] = CRGB::White;   // the rim rings
  }
}

static void kFireworks(CRGB *buf, uint32_t t, uint8_t level) { fireworks(buf, t, level); }   // team colours

static String kScore(const GameEvent &e) { return scoreLine(e); }

// NYC skyline: building tops per column (lower = taller); the Empire State
// Building is the two middle columns, its crown on rows 3-5 and a spire above.
#if GRID_W == 32
static const uint8_t SKYLINE[W] = {11, 9, 9, 12, 10, 6, 6, 11, 10, 10, 12, 8, 8, 11, 7, 3,
                                   3, 7, 11, 9, 9, 12, 7, 7, 10, 8, 8, 12, 9, 5, 5, 11};
#else
static const uint8_t SKYLINE[W] = {10, 10, 12, 8, 8, 11, 7, 3, 3, 7, 11, 9, 9, 12, 7, 7};
#endif
static_assert(sizeof(SKYLINE) == W, "a skyline column for every x");
constexpr int ESB = W / 2 - 1;        // the Empire State Building's left column

static void skyline(CRGB *buf, uint32_t t, uint8_t lightsOn, bool crown) {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) buf[y * W + x] = blend(CRGB(0, 0, 12), CRGB(8, 5, 30), y * 255 / H);
  for (int x = 0; x < W; x++)
    for (int y = SKYLINE[x]; y < H; y++) {
      buf[y * W + x] = CRGB(38, 44, 66);   // buildings, a shade lighter than the sky
      uint32_t h = (x * 31 + y * 17) * 2654435761u;
      if ((h >> 24) < lightsOn && x % 2 == 0 && y % 2 && y > SKYLINE[x]) buf[y * W + x] = CRGB(150, 120, 40);   // windows
    }
  buf[1 * W + ESB] = buf[2 * W + ESB] = buf[1 * W + ESB + 1] = buf[2 * W + ESB + 1] = CRGB(60, 60, 70);   // spire
  if (crown) {                                       // the ESB crown chases blue and orange
    for (int y = 3; y <= 5; y++)
      for (int x = ESB; x <= ESB + 1; x++) buf[y * W + x] = ((y + x + t / 200) % 2) ? TC1 : TC2;
  }
}

static bool knicksWin(CRGB *buf, uint32_t t) {
  bool nyc = isKnicks(showTeam);
  if (!nyc && t < 3000) { fill_solid(buf, NUM_LEDS, CRGB::Black); kFireworks(buf, t, 255); return true; }
  if (!nyc && t >= 3000) t += 4000;                  // others: 3 s of fireworks, then the words
  if (t < 3000) { skyline(buf, t, 140, true); return true; }
  if (t < 7000) { skyline(buf, t, 140, true); kFireworks(buf, t - 3000, 255); return true; }
  uint32_t s = t - 7000;
  const String words = nyc ? String("KNICKS WIN!") : String(showTeam.abbr) + " WIN!";
  uint32_t scoreT0 = (textWidth(words, 2) + W) * 35;
  for (int y = 0; y < H; y++)                        // blue and orange waves
    for (int x = 0; x < W; x++) {
      uint8_t v = sin8(x * 20 + y * 12 + t / 4);
      buf[y * W + x] = blend(TC1, TC2, v).nscale8(s < scoreT0 ? 120 : 50);
    }
  kFireworks(buf, s, 70);
  if (s < scoreT0) return scrollOnce(buf, words, s, 0, 2, 35, CRGB::White);
  return scrollOnce(buf, kScore(show), s, scoreT0, 1, 55, CRGB::White);
}

static bool knicksLoss(CRGB *buf, uint32_t t) {
  if (!isKnicks(showTeam)) return lossShow(buf, t + 4000, "FINAL");   // the tombstone
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
      if (x >= OX + 4 && x <= OX + 11 && random8() < 200) heat[x][random8(4, 7)] = qadd8(heat[x][4], random8(170, 255));
    }
    for (int x = 0; x < W; x++)
      for (int y = 0; y < H; y++) buf[(H - 1 - y) * W + x] = ColorFromPalette(HeatColors_p, scale8(heat[x][y], 230));
    int bounce = abs((int)((t / 30) % 8) - 4) / 2;   // the ball bobs in the flames
    ball(buf, OX + 5, 9 - bounce, 6);
    return true;
  }
  memset(heat, 0, sizeof(heat));
  uint32_t s = t - 3200;
  String words = String(show.extra) + "-0 RUN";
  uint32_t textEnd = (textWidth("ON FIRE", 1) + W) * 45, runEnd = textEnd + (textWidth(words, 2) + W) * 40;
  for (int i = 0; i < NUM_LEDS; i++) buf[i] = CRGB(TC1).scale8(70 + scale8(sin8(i * 9 + t / 3), 50));
  if (s < textEnd) return scrollOnce(buf, "ON FIRE", s, 0, 1, 45, TC2);
  return scrollOnce(buf, words, s, textEnd, 2, 40, CRGB::White) || false;
}

static bool knicksLead(CRGB *buf, uint32_t t) {
  for (int y = 0; y < H; y++)                        // stripes marching diagonally
    for (int x = 0; x < W; x++) buf[y * W + x] = ((x + y + t / 60) / 3) % 2 ? TC1 : TC2;
  if (t < 1400) return true;
  nscale8(buf, NUM_LEDS, 60);
  String words = isKnicks(showTeam) ? String("KNICKS LEAD") : String(showTeam.abbr) + " LEAD";
  return scrollOnce(buf, words, t - 1400, 0, 1, 45, CRGB::White);
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
    for (int i = 0; i < NUM_LEDS; i++) buf[i] = CRGB(TC1).scale8(scale8(pulse, 120));
    hoop(buf, pulse);
    plusPoints(buf, show.extra ? show.extra : 2, TC2);
    return true;
  }
  if (s > 2000) return false;                        // three: a burst and a big 3
  float r = s / 2000.0f * (CX + 4.5f);
  for (int i = 0; i < NUM_LEDS; i++) {
    float d = fabsf(DIST[i] / 16.0f - r);
    buf[i] = d < 1.5f ? ((s / 250) % 2 ? TC2 : TC1) : CRGB::Black;
  }
  drawText(buf, "3", (W - 10) / 2, 1, 2, CRGB::White);
  return true;
}

// Opponent basket: the current animation dims, with a red edge flicker.
static bool knicksOpp(CRGB *buf, uint32_t t) {
  if (t > 900) return false;
  memcpy(buf, frame, sizeof(CRGB) * NUM_LEDS);
  nscale8(buf, NUM_LEDS, 90);
  if ((t / 150) % 2 == 0) {
    for (int x = 0; x < W; x++) buf[x] = buf[(H - 1) * W + x] = CRGB(140, 0, 0);
    for (int y = 0; y < H; y++) buf[y * W] = buf[y * W + W - 1] = CRGB(140, 0, 0);
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

// ---- Scoreboard: the game screen for basketball, football and baseball ------
// The live score (ours in the team colour on top, theirs grey below) with a
// ball bouncing across between them: a basketball, a football or a baseball.
static void drawScoreRow(CRGB *buf, int score, int y, CRGB c) {
  String s = String(score);
  drawDigits(buf, s, (W - digitsWidth(s)) / 2, y, c);
}

static void fxCourt() {
  int slot = liveSlot();
  Team t = teamAt(slot >= 0 ? slot : 0);
  useTeam(t);
  uint32_t now = millis();
  bool stale = slot >= 0 && now - games[slot].scoreAt > 60000;   // see fxPong
  fill_solid(frame, NUM_LEDS, COURT_BG);
  for (int x = 0; x < W; x++) frame[8 * W + x] = CRGB(10, 14, 40);   // half-way line
  if (!stale || (now / 500) % 2) {
    drawScoreRow(frame, slot >= 0 ? games[slot].us : 0, 1, TC1);
    drawScoreRow(frame, slot >= 0 ? games[slot].them : 0, 10, KSILVER);
  }
  frame[3 * W + 0] = TC2; frame[12 * W + 0] = CRGB(60, 60, 70);         // team markers
  const int span = W - 3;                                             // 13 on one panel
  int x = (int)((now / 90) % (2 * span));
  x = x < span ? x : 2 * span - x;                                    // back and forth
  int y = 6 + abs((int)((now / 70) % 4) - 2);                         // bouncing: rows 6..9
  CRGB top = t.league == L_MLB ? CRGB(230, 230, 230) : t.league == L_NFL ? CRGB(120, 55, 15) : BALL;
  CRGB bottom = t.league == L_MLB ? CRGB(150, 30, 30) : t.league == L_NFL ? CRGB(70, 30, 8) : CRGB(180, 70, 10);
  frame[y * W + x + 1] = frame[y * W + x + 2] = top;
  if (y + 1 < H) frame[(y + 1) * W + x + 1] = frame[(y + 1) * W + x + 2] = bottom;
}
