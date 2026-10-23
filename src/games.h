// Following up to two teams, any of NHL, NBA, NFL or MLB. Included once, from
// main.cpp, after the drawing helpers and before the shows (sharks.h, knicks.h).
//
// Data: ESPN's public API, no key, the same calls for every league. A task on
// core 0 checks each team's page (nextEvent: the next or current game) every
// 2 minutes; while a game is live it polls each side's score (~250 B) every
// 6 s and the game status every 18 s from ESPN's core API. Score changes
// become events on a queue; the render loop on core 1 plays them as shows.
//
// Teams are chosen on the web page, which carries ESPN's team list (see
// tools/teams.py) and sends "league:id:abbr:color1:color2".
#pragma once
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

enum League : uint8_t { L_NONE, L_NHL, L_NBA, L_NFL, L_MLB, L_COUNT };
static const char *LEAGUE_KEYS[L_COUNT] = {"", "nhl", "nba", "nfl", "mlb"};
static const char *LEAGUE_SPORTS[L_COUNT] = {"", "hockey", "basketball", "football", "baseball"};

struct Team {
  League league = L_NONE;
  uint16_t id = 0;                // ESPN team id
  char abbr[5] = "";              // ESPN's abbreviation: SJ, NY, DEN...
  CRGB c1 = CRGB::White, c2 = CRGB::White;   // LED-ready team colours
};
constexpr int SLOTS = 2;
static Team teams[SLOTS];         // guarded by gameLock

// The two teams with their own custom shows (shark, NYC skyline).
static bool isSharks(const Team &t) { return t.league == L_NHL && t.id == 18; }
static bool isKnicks(const Team &t) { return t.league == L_NBA && t.id == 18; }

enum ShowKind : uint8_t {
  SHOW_NONE, SHOW_GOAL, SHOW_AGAINST, SHOW_WIN, SHOW_LOSS,           // every league; GOAL = we scored
  K_SWISH, K_THREE, K_OPP, K_RUN, K_LEAD, K_BRICK, K_WIN, K_LOSS,    // basketball (knicks.h)
};

struct GameEvent {
  ShowKind kind;
  uint8_t slot;
  uint16_t us, them;
  char opp[5];
  uint8_t extra;      // points scored (or the length of a run)
};

static QueueHandle_t gameEvents;
static SemaphoreHandle_t gameLock;

// What the render loop needs about each slot's game. Written on core 0.
struct LiveGame {
  volatile bool live = false;
  volatile uint16_t us = 0, them = 0;
  volatile uint32_t scoreAt = 0;  // millis() of the last good score fetch
  char opp[5] = "";
  String status = "{\"state\":\"idle\"}";   // JSON for the page; guarded by gameLock
};
static LiveGame games[SLOTS];
static volatile bool teamsChanged = true;

// "nhl:18:SJ:00a0af:ea5a00" -> a Team. "" (or junk) is no team.
static Team parseTeam(const String &s) {
  Team t;
  int a = s.indexOf(':'), b = s.indexOf(':', a + 1), c = s.indexOf(':', b + 1), d = s.indexOf(':', c + 1);
  if (a < 0 || b < 0 || c < 0 || d < 0) return t;
  String lg = s.substring(0, a);
  for (uint8_t i = 1; i < L_COUNT; i++) if (lg == LEAGUE_KEYS[i]) t.league = (League)i;
  t.id = s.substring(a + 1, b).toInt();
  strlcpy(t.abbr, s.substring(b + 1, c).c_str(), sizeof(t.abbr));
  t.c1 = colorOf(strtoul(s.substring(c + 1, d).c_str(), nullptr, 16));
  t.c2 = colorOf(strtoul(s.substring(d + 1).c_str(), nullptr, 16));
  if (!t.id || !t.abbr[0]) t.league = L_NONE;
  return t;
}

static Team teamAt(int slot) {
  xSemaphoreTake(gameLock, portMAX_DELAY);
  Team t = teams[slot];
  xSemaphoreGive(gameLock);
  return t;
}

static void setTeams(const String specs[SLOTS]) {
  xSemaphoreTake(gameLock, portMAX_DELAY);
  for (int i = 0; i < SLOTS; i++) {
    Team t = parseTeam(specs[i]);
    // A new team: drop the old one's game at once, so the page never shows it.
    if (t.league != teams[i].league || t.id != teams[i].id) {
      games[i].live = false;
      games[i].status = t.league ? "{\"state\":\"idle\"}" : "{\"state\":\"off\"}";
    }
    teams[i] = t;
  }
  xSemaphoreGive(gameLock);
  teamsChanged = true;
}

static void setStatus(int slot, const String &json) {
  xSemaphoreTake(gameLock, portMAX_DELAY);
  games[slot].status = json;
  xSemaphoreGive(gameLock);
}

// The page's view of both slots: the team, then its game.
static String teamsJson() {
  String j = "[";
  xSemaphoreTake(gameLock, portMAX_DELAY);
  for (int i = 0; i < SLOTS; i++) {
    const Team &t = teams[i];
    char c1[7], c2[7];
    snprintf(c1, sizeof(c1), "%02x%02x%02x", t.c1.r, t.c1.g, t.c1.b);
    snprintf(c2, sizeof(c2), "%02x%02x%02x", t.c2.r, t.c2.g, t.c2.b);
    j += String(i ? "," : "") + "{\"league\":\"" + LEAGUE_KEYS[t.league] + "\",\"id\":" + t.id +
         ",\"abbr\":\"" + t.abbr + "\",\"c1\":\"" + c1 + "\",\"c2\":\"" + c2 + "\",\"game\":" + games[i].status + "}";
  }
  xSemaphoreGive(gameLock);
  return j + "]";
}

// The first slot with a live game, or -1. The render loop's game screen.
static int liveSlot() {
  for (int i = 0; i < SLOTS; i++) if (games[i].live) return i;
  return -1;
}

static void pushEvent(ShowKind kind, int slot, int us, int them, const char *opp, int extra = 0) {
  GameEvent e{kind, (uint8_t)slot, (uint16_t)us, (uint16_t)them, {0}, (uint8_t)min(extra, 255)};
  strlcpy(e.opp, opp, sizeof(e.opp));
  xQueueSend(gameEvents, &e, 0);
}

// ---- Fetching (core 0) ------------------------------------------------------
static bool fetchJson(const String &url, JsonDocument &doc, const JsonDocument &filter) {
  WiFiClientSecure client;
  client.setInsecure();                    // public scores; skip certificate checks
  HTTPClient http;
  http.useHTTP10(true);                    // no chunked encoding, so the stream parses directly
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(4000);                   // the parser spins this long on a stall; keep it under the 5 s watchdog
  http.setUserAgent("led-grid/1.0 (ESP32)");   // adsb.lol turns away the default one
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) ok = !deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter),
                     DeserializationOption::NestingLimit(32));   // ESPN nests past the default 10
  else Serial.printf("GET %s -> HTTP %d\n", url.c_str(), code);
  http.end();
  return ok;
}

// One kept-alive HTTPS connection for the tiny core-API requests, so a live
// game costs one TLS handshake rather than one every few seconds.
static bool fetchCore(const String &url, JsonDocument &doc, const JsonDocument *filter = nullptr) {
  static WiFiClientSecure client;
  static HTTPClient http;
  static bool init = false;
  if (!init) { client.setInsecure(); http.setReuse(true); http.setTimeout(4000); init = true; }
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    ok = filter ? !deserializeJson(doc, body, DeserializationOption::Filter(*filter), DeserializationOption::NestingLimit(32))
                : !deserializeJson(doc, body, DeserializationOption::NestingLimit(32));
  } else {
    Serial.printf("ESPN %s -> HTTP %d\n", url.c_str(), code);
    http.end();
    client.stop();
  }
  return ok;
}

// ESPN sends scores as decimals ("value": 3.0). ArduinoJson's `| 0` treats a
// decimal as "not an int" and quietly gives 0, so read it as a number.
static int scoreOf(JsonDocument &d) { return lroundf(d["value"].as<float>()); }

// ---- Following each slot (core 0) ---------------------------------------------
struct Follow {                   // the task's private state per slot
  Team team;
  String eid, usId, themId, opp, detail, preEid;
  bool home = false, live = false;
  int us = -1, them = -1, lead = 0, usRun = 0, themRun = 0;
  bool usRunShown = false, themRunShown = false;
  uint32_t nextCheck = 0, nextPoll = 0, lastStatus = 0, lastBig = 0;
};
static Follow follows[SLOTS];

static String coreBase(const Follow &f) {
  return String("https://sports.core.api.espn.com/v2/sports/") + LEAGUE_SPORTS[f.team.league] + "/leagues/" +
         LEAGUE_KEYS[f.team.league] + "/events/" + f.eid + "/competitions/" + f.eid;
}

static void liveStatus(int slot, const Follow &f, bool over) {
  setStatus(slot, "{\"state\":\"" + String(over ? "final" : "live") + "\",\"us\":" + f.us + ",\"them\":" + f.them +
                  ",\"opp\":\"" + f.opp + "\",\"home\":" + (f.home ? "true" : "false") + ",\"detail\":\"" + f.detail + "\"}");
}

static void goLive(int slot, Follow &f, const String &eid, const String &usId, const String &themId,
                   const String &opp, bool home) {
  Serial.printf("%s game %s live vs %s\n", f.team.abbr, eid.c_str(), opp.c_str());
  // Seen before the start: count every score from 0-0. Joined mid-game:
  // take the first scores as they are, so old goals don't replay.
  bool fromStart = f.preEid == eid;
  f.eid = eid; f.usId = usId; f.themId = themId; f.opp = opp; f.home = home;
  f.us = fromStart ? 0 : -1; f.them = fromStart ? 0 : -1;
  f.lead = 0; f.usRun = f.themRun = 0; f.usRunShown = f.themRunShown = false;
  f.detail = ""; f.lastStatus = 0; f.nextPoll = 0;
  f.live = true;
  strlcpy(games[slot].opp, opp.c_str(), sizeof(games[slot].opp));
}

static bool clockSet() { return time(nullptr) > 1700000000; }

// The team's games from yesterday to tomorrow (UTC) on ESPN's core API, a few
// hundred bytes. Any other than the team page's nextEvent that is in progress
// is followed. Needs the clock, which the quarter-hour card sets over NTP.
static bool findLiveToday(int slot, Follow &f, const String &nextEid) {
  time_t now = time(nullptr);
  if (!clockSet()) return false;
  char from[9], to[9];
  time_t a = now - 86400, b = now + 86400;
  struct tm tm;
  strftime(from, sizeof(from), "%Y%m%d", gmtime_r(&a, &tm));
  strftime(to, sizeof(to), "%Y%m%d", gmtime_r(&b, &tm));
  const Team &t = f.team;
  String lg = String("https://sports.core.api.espn.com/v2/sports/") + LEAGUE_SPORTS[t.league] + "/leagues/" +
              LEAGUE_KEYS[t.league];
  static JsonDocument listFilter, statusFilter, compFilter, teamFilter;
  if (listFilter.isNull()) {
    listFilter["items"][0]["$ref"] = true;
    statusFilter["type"]["state"] = true;
    JsonObject who = compFilter["competitors"].add<JsonObject>();
    who["id"] = true;
    who["homeAway"] = true;
    teamFilter["abbreviation"] = true;
  }
  JsonDocument list;
  if (!fetchCore(lg + "/teams/" + t.id + "/events?dates=" + from + "-" + to, list, &listFilter)) return false;
  for (JsonObject it : list["items"].as<JsonArray>()) {
    String ref = it["$ref"] | "";         // .../events/401892554?lang=en&region=us
    int at = ref.indexOf("/events/");
    if (at < 0) continue;
    String eid = ref.substring(at + 8);
    int q = eid.indexOf('?');
    if (q >= 0) eid = eid.substring(0, q);
    if (eid == nextEid) continue;
    String comp = lg + "/events/" + eid + "/competitions/" + eid;
    JsonDocument st;
    if (!fetchCore(comp + "/status", st, &statusFilter) || strcmp(st["type"]["state"] | "", "in")) continue;
    JsonDocument c;
    if (!fetchCore(comp, c, &compFilter)) continue;
    String usId, themId;
    bool home = false;
    for (JsonObject who : c["competitors"].as<JsonArray>()) {
      String cid = who["id"] | "";
      if (cid.toInt() == t.id) { usId = cid; home = !strcmp(who["homeAway"] | "", "home"); }
      else themId = cid;
    }
    if (!usId.length() || !themId.length()) continue;
    JsonDocument them;
    String opp = fetchCore(lg + "/teams/" + themId, them, &teamFilter) ? String(them["abbreviation"] | "???") : "???";
    goLive(slot, f, eid, usId, themId, opp, home);
    return true;
  }
  return false;
}

// The team's next (or current) game, from its ESPN team page.
static void checkSchedule(int slot, Follow &f) {
  static JsonDocument filter;
  if (filter.isNull()) {
    JsonObject ev = filter["team"]["nextEvent"].add<JsonObject>();
    ev["id"] = true;
    ev["date"] = true;
    JsonObject comp = ev["competitions"].add<JsonObject>();
    comp["status"]["type"]["state"] = true;
    JsonObject who = comp["competitors"].add<JsonObject>();
    who["id"] = true;
    who["homeAway"] = true;
    who["team"]["abbreviation"] = true;
    who["score"]["displayValue"] = true;
  }
  const Team &t = f.team;
  String url = String("https://site.api.espn.com/apis/site/v2/sports/") + LEAGUE_SPORTS[t.league] + "/" +
               LEAGUE_KEYS[t.league] + "/teams/" + t.id;
  JsonDocument doc;
  if (!fetchJson(url, doc, filter)) { f.nextCheck = millis() + 30000; return; }
  JsonObject e = doc["team"]["nextEvent"][0];
  if (e.isNull()) { setStatus(slot, "{\"state\":\"none\"}"); f.nextCheck = millis() + 600000; return; }

  String eid = e["id"] | "";
  const char *state = e["competitions"][0]["status"]["type"]["state"] | "pre";
  String usId, themId, opp = "???", usScore, themScore;
  bool home = false;
  for (JsonObject c : e["competitions"][0]["competitors"].as<JsonArray>()) {
    String cid = c["id"] | "";
    if (cid.toInt() == t.id) { usId = cid; home = !strcmp(c["homeAway"] | "", "home"); usScore = c["score"]["displayValue"] | ""; }
    else { themId = cid; opp = c["team"]["abbreviation"] | "???"; themScore = c["score"]["displayValue"] | ""; }
  }
  if (!strcmp(state, "in") && usId.length() && themId.length()) {
    goLive(slot, f, eid, usId, themId, opp, home);
    return;
  }
  // ESPN moves nextEvent on to the following game once one starts, so a game
  // in progress can be missing from the team page. Look at today's games too.
  if (strcmp(state, "post") && findLiveToday(slot, f, eid)) return;
  if (!strcmp(state, "post")) {
    setStatus(slot, "{\"state\":\"final\",\"us\":" + String(usScore.length() ? usScore : "0") + ",\"them\":" +
                    String(themScore.length() ? themScore : "0") + ",\"opp\":\"" + opp + "\",\"home\":" +
                    (home ? "true" : "false") + ",\"detail\":\"Final\"}");
  } else {
    f.preEid = eid;
    setStatus(slot, "{\"state\":\"next\",\"opp\":\"" + opp + "\",\"home\":" + (home ? "true" : "false") +
                    ",\"start\":\"" + String(e["date"] | "") + "\"}");
  }
  f.nextCheck = millis() + (clockSet() ? 120000 : 15000);   // soon again if today's games went unchecked
}

// Basketball scores ~45 times a game per side, so its shows are tiered.
static void basketballEvents(int slot, Follow &f, int nowUs, int nowThem) {
  const char *opp = f.opp.c_str();
  bool bigOk = millis() - f.lastBig > 90000;      // runs and leads: 90 s apart at least
  int dUs = nowUs - f.us, dThem = nowThem - f.them;
  if (dUs > 0) {
    pushEvent(dUs == 3 ? K_THREE : K_SWISH, slot, nowUs, nowThem, opp, dUs);
    f.usRun += dUs; f.themRun = 0; f.themRunShown = false;
    if (f.usRun >= 8 && !f.usRunShown && bigOk) { pushEvent(K_RUN, slot, nowUs, nowThem, opp, f.usRun); f.usRunShown = true; f.lastBig = millis(); bigOk = false; }
  }
  if (dThem > 0) {
    pushEvent(K_OPP, slot, nowUs, nowThem, opp, dThem);
    f.themRun += dThem; f.usRun = 0; f.usRunShown = false;
    if (f.themRun >= 8 && !f.themRunShown && bigOk) { pushEvent(K_BRICK, slot, nowUs, nowThem, opp, f.themRun); f.themRunShown = true; f.lastBig = millis(); bigOk = false; }
  }
  int nowLead = nowUs - nowThem;
  if (bigOk && f.lead <= 0 && nowLead > 0) { pushEvent(K_LEAD, slot, nowUs, nowThem, opp); f.lastBig = millis(); }
  else if (bigOk && f.lead >= 0 && nowLead < 0) { pushEvent(K_BRICK, slot, nowUs, nowThem, opp, 0); f.lastBig = millis(); }
  f.lead = nowLead;
}

static void pollLive(int slot, Follow &f) {
  String base = coreBase(f);
  JsonDocument a, b;
  if (!fetchCore(base + "/competitors/" + f.usId + "/score", a) ||
      !fetchCore(base + "/competitors/" + f.themId + "/score", b)) return;   // a blip: try again next poll
  int nowUs = scoreOf(a), nowThem = scoreOf(b);
  if (f.us < 0) { f.us = nowUs; f.them = nowThem; f.lead = nowUs - nowThem; }   // joined mid-game: no replays

  if (f.team.league == L_NBA) {
    basketballEvents(slot, f, nowUs, nowThem);
  } else {
    // A score overturned on review just lowers the number quietly.
    if (nowUs > f.us) pushEvent(SHOW_GOAL, slot, nowUs, nowThem, f.opp.c_str(), nowUs - f.us);
    if (nowThem > f.them) pushEvent(SHOW_AGAINST, slot, nowUs, nowThem, f.opp.c_str(), nowThem - f.them);
  }
  f.us = nowUs; f.them = nowThem;
  LiveGame &g = games[slot];
  g.us = nowUs; g.them = nowThem; g.scoreAt = millis();
  g.live = true;

  bool over = false;
  if (millis() - f.lastStatus > 18000 || f.lastStatus == 0) {
    static JsonDocument filter;
    if (filter.isNull()) { filter["type"]["state"] = true; filter["type"]["shortDetail"] = true; }
    JsonDocument st;
    if (fetchCore(base + "/status", st, &filter)) {
      f.lastStatus = millis();
      over = !strcmp(st["type"]["state"] | "in", "post");
      f.detail = st["type"]["shortDetail"] | "";
      f.detail.replace("\"", "");
    }
  }
  liveStatus(slot, f, over);
  if (!over) return;

  // Final. Remember the game so a reboot doesn't replay the result.
  Preferences p;
  p.begin("games", false);
  String key = "done" + String(slot);
  if (p.getString(key.c_str(), "") != f.eid) {
    p.putString(key.c_str(), f.eid);
    bool won = f.us > f.them;
    ShowKind k = f.team.league == L_NBA ? (won ? K_WIN : K_LOSS) : (won ? SHOW_WIN : SHOW_LOSS);
    pushEvent(k, slot, f.us, f.them, f.opp.c_str());
  }
  p.end();
  f.live = false;
  g.live = false;
  f.nextCheck = millis() + 300000;    // the finished game stays "next" on ESPN for a while
}

static void gamesTask(void *) {
  for (;;) {
    if (teamsChanged) {
      teamsChanged = false;
      for (int i = 0; i < SLOTS; i++) {
        Team t = teamAt(i);
        Follow &f = follows[i];
        if (t.league == f.team.league && t.id == f.team.id) { f.team = t; continue; }   // only colours changed
        f = Follow();
        f.team = t;
        games[i].live = false;
        setStatus(i, t.league ? "{\"state\":\"idle\"}" : "{\"state\":\"off\"}");
      }
    }
    if (WiFi.status() != WL_CONNECTED) {
      for (int i = 0; i < SLOTS; i++) if (follows[i].team.league) setStatus(i, "{\"state\":\"offline\"}");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    for (int i = 0; i < SLOTS; i++) {
      Follow &f = follows[i];
      if (!f.team.league) continue;
      uint32_t now = millis();
      if (f.live) {
        if ((int32_t)(now - f.nextPoll) >= 0) { f.nextPoll = now + 6000; pollLive(i, f); }
      } else if ((int32_t)(now - f.nextCheck) >= 0) {
        checkSchedule(i, f);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

static void setupGames(const String specs[SLOTS]) {
  gameEvents = xQueueCreate(8, sizeof(GameEvent));
  gameLock = xSemaphoreCreateMutex();
  setTeams(specs);
  // Priority 0, the same as the idle task: Stream::timedRead busy-waits while a
  // response stalls, and above idle that starves IDLE0 into a task-watchdog reset.
  xTaskCreatePinnedToCore(gamesTask, "games", 12288, nullptr, 0, nullptr, 0);
}
