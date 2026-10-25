// Flights mode: the plane nearest home, from ADS-B. Included once, from
// main.cpp, after ambient.h.
//
// Each round: the airline's logo fills the panel for 3 s (16x16 pixel art for
// the airlines drawn here, otherwise its two-letter code on its colour; none
// for private planes). Then the details: top left, a plane pointing the way
// it's flying (north up); top right, its altitude in thousands of feet,
// coloured low orange to high blue, over a bar that fills as it nears; along
// the bottom, airline and flight number, route, aircraft, altitude, distance
// and speed scroll by once. A new nearest plane takes over at the next round.
//
// Data, fetched on core 0 every 10 s while the mode is on screen: positions
// from adsb.fi (adsb.lol if that fails), no key. Airline and route come from
// adsbdb.com once per callsign. Route databases go stale when airlines reuse
// flight numbers, so a route only shows while the plane is roughly between
// the two airports and heading for the second. Home is the weather location (Setup), rounded to ~1 km.
#pragma once

constexpr int FLIGHT_RADIUS_NM = 25;
constexpr uint32_t FLIGHT_POLL_MS = 10000;

struct Flight {
  bool valid = false;
  char hex[7] = "";
  char callsign[9] = "";     // "SWA3091", or a registration for private planes
  char type[24] = "";        // "BOEING 737 MAX 8", or the ICAO code "B38M"
  char airline[28] = "";     // from adsbdb when the table below doesn't know it
  char iata[3] = "";         // two-letter airline code ("WN"), for the card
  char route[8] = "";        // "LAS>RNO", only when plausible
  int altFt = 0;
  int mph = 0;
  float miles = 0;
  float track = 0;           // degrees, 0 = north
};

struct FlightSky {
  bool fetched = false;      // at least one answer since the mode came on
  uint8_t count = 0;         // airborne aircraft in range
  Flight nearest;
};
static FlightSky sky;        // written on core 0 under skyLock, copied out to read
static SemaphoreHandle_t skyLock;
static volatile uint32_t flightsSeenAt = 0;   // millis() the effect last drew

static FlightSky currentSky() {
  xSemaphoreTake(skyLock, portMAX_DELAY);
  FlightSky s = sky;
  xSemaphoreGive(skyLock);
  return s;
}

// ---- Airlines -----------------------------------------------------------------
// Logos: 16 rows of 16, '.' = off, other letters looked up in keys/colors.
// Drawn by hand at the panel's size. To see one on the grid:
// /api/flight/test?callsign=UAL1.
struct Logo { const char *px; const char *keys; uint32_t colors[4]; };
enum : uint8_t { LG_NONE, LG_SOUTHWEST, LG_UNITED, LG_AMERICAN, LG_DELTA, LG_ALASKA, LG_JETBLUE, LG_FEDEX, LG_UPS, LG_SPIRIT };
static const Logo LOGOS[] = {
  {nullptr, "", {}},
  {   // Southwest: the heart
   "................"
   "..bbbb....bbbb.."
   ".bbbbbb..bbbbbb."
   "bbbbbbbbbbbbbbbb"
   "bbbbbbbbbbbbbbbb"
   "bbbbbbbbbbbbbbbr"
   "bbbbbbbbbbbrrrrr"
   "bbbbbbbrrrrrrrrr"
   ".bbbrrrrrrrrrrr."
   "..rrrrrrrrrrryy."
   "...rrrrrrryyy..."
   "....rrryyyyy...."
   ".....yyyyyy....."
   "......yyyy......"
   ".......yy......."
   "................"
   , "bry", {0x1A3CC8, 0xFF1030, 0xFFB000}},
  {   // United: the globe
   "......bbbb......"
   "....bbbbbbbb...."
   "..bllllllllllb.."
   "..bbbbbbbbbbbb.."
   ".llbbbbbbbbbbll."
   ".bbllllllllllbb."
   "bbbbbbbbbbbbbbbb"
   "bbbbbbbbbbbbbbbb"
   "llllllllllllllll"
   "bbbbbbbbbbbbbbbb"
   ".bbllllllllllbb."
   ".llbbbbbbbbbbll."
   "..bbbbbbbbbbbb.."
   "..bllllllllllb.."
   "....bbbbbbbb...."
   "......bbbb......"
   , "bl", {0x0020A0, 0x60B0FF}},
  {   // American: the flag on the tail
   ".........bbbbbbb"
   "........bbbbbbbb"
   ".......bbbbbbbbb"
   "......bbbbbbbbbb"
   ".....bbbbbbbbbbb"
   "....bbbbbbbbbbbr"
   "...bbbbbbbbbbrrw"
   "..bbbbbbbbbrrwwr"
   ".bbbbbbbbrrwwrrw"
   "bbbbbbbrrwwrrwwr"
   "bbbbbrrwwrrwwrrw"
   "bbbrrwwrrwwrrwwr"
   "brrwwrrwwrrwwrrw"
   "rwwrrwwrrwwrrwwr"
   "wrrwwrrwwrrwwrrw"
   "................"
   , "bwr", {0x1040FF, 0xB0B0B0, 0xFF1020}},
  {   // Delta: the widget
   "................"
   ".......dr......."
   ".......drr......"
   "......ddrr......"
   "......ddrrr....."
   ".....dddrrr....."
   ".....dddrrrr...."
   "....ddddrrrr...."
   "....ddddrrrrr..."
   "...dddddrrrrr..."
   "...dddddrrrrrr.."
   "..ddddddrrrrrr.."
   "..ddddddrrrrrrr."
   ".dddddddrrrrrrr."
   ".dddddddrrrrrrrr"
   "................"
   , "dr", {0x6A0010, 0xFF1830}},
  {   // Alaska (and Horizon): the face in the parka
   "....wtwtwtwt...."
   "..twwwwwwwwwwt.."
   ".twwnnnnnnnnwwt."
   ".wwnnnnnnnnnnww."
   "twnnssssssssnnwt"
   "wwnssssssssssnww"
   "twnssnssssnssnwt"
   "wwnssnssssnssnww"
   "twnssssssssssnwt"
   "wwnssssssssssnww"
   "twnsnssssssnsnwt"
   ".wwnssnnnnssnww."
   ".twwnssssssnwwt."
   "..twwnnnnnnwwt.."
   "....wtwtwtwt...."
   "................"
   , "wtns", {0xC8C8C8, 0x00A0B0, 0x002070, 0xC07040}},
  {   // JetBlue
   "nnnnnnnnnnnnnnnn"
   "nnnnnnnnnnnnnnnn"
   "nnnnnnnnnnnnnnnn"
   "nnnnlnnlllllnnnn"
   "nnnnnnnlnnnnlnnn"
   "nnnnlnnlnnnnlnnn"
   "nnnnlnnlllllnnnn"
   "nnnnlnnlnnnnlnnn"
   "nnnnlnnlnnnnlnnn"
   "nnnnlnnlnnnnlnnn"
   "nnnnlnnlllllnnnn"
   "nnnnlnnnnnnnnnnn"
   "nnllnnnnnnnnnnnn"
   "nnnnnnnnnnnnnnnn"
   "nnnnnnnnnnnnnnnn"
   "nnnnnnnnnnnnnnnn"
   , "nl", {0x001A60, 0x40A0FF}},
  {   // FedEx
   "................"
   "................"
   "................"
   "..ppp.......p..."
   "..p....p....p..."
   "..pp..ppp..pp..."
   "..p...p...p.p..."
   "..p....pp..pp..."
   "................"
   "......ooo.o.o..."
   "......o...o.o..."
   "......oo...o...."
   "......o...o.o..."
   "......ooo.o.o..."
   "................"
   "................"
   , "po", {0x7020D0, 0xFF6000}},
  {   // UPS: the shield
   "................"
   ".gggggggggggggg."
   ".gbbbbbbbbbbbbg."
   ".bbbbbbbbbbbbbb."
   ".bbbbbbbbbbbbbb."
   ".bbgbgbgggbgggb."
   ".bbgbgbgbgbgbbb."
   ".bbgbgbgggbgggb."
   ".bbgbgbgbbbbbgb."
   ".bbgggbgbbbgggb."
   ".bbbbbbbbbbbbbb."
   ".bbbbbbbbbbbbbb."
   "..bbbbbbbbbbbb.."
   "...bbbbbbbbbb..."
   ".....bbbbbb....."
   ".......bb......."
   , "bg", {0x4A2200, 0xFFB000}},
  {   // Spirit
   "yyyyyyyyyyyyyyyy"
   "yyyyyyyyyyyyyyyy"
   "yyyyykkkkkkkyyyy"
   "yyyykkkkkkkkyyyy"
   "yyyykkyyyyyyyyyy"
   "yyyykkyyyyyyyyyy"
   "yyyykkkkkkkyyyyy"
   "yyyyykkkkkkkyyyy"
   "yyyyyyyyyyykkyyy"
   "yyyyyyyyyyykkyyy"
   "yyyyyyyyyyykkyyy"
   "yyyykkkkkkkkkyyy"
   "yyyykkkkkkkkyyyy"
   "yyyyyyyyyyyyyyyy"
   "yyyyyyyyyyyyyyyy"
   "yyyyyyyyyyyyyyyy"
   , "yk", {0xFFD000, 0x000000}},
};

// Without a logo, the airline's two-letter code on its colour stands in.
struct Airline { char icao[4]; char iata[3]; const char *name; uint8_t logo; uint32_t color; };
static const Airline AIRLINES[] = {
  {"SWA", "WN", "SOUTHWEST", LG_SOUTHWEST, 0xFFB000}, {"UAL", "UA", "UNITED", LG_UNITED, 0x3080FF},
  {"AAL", "AA", "AMERICAN", LG_AMERICAN, 0xC0C8D0}, {"ENY", "MQ", "AMERICAN EAGLE", LG_AMERICAN, 0xC0C8D0},
  {"PDT", "PT", "AMERICAN EAGLE", LG_AMERICAN, 0xC0C8D0}, {"JIA", "OH", "AMERICAN EAGLE", LG_AMERICAN, 0xC0C8D0},
  {"DAL", "DL", "DELTA", LG_DELTA, 0xFF3040}, {"EDV", "9E", "DELTA CONNECTION", LG_DELTA, 0xFF3040},
  {"ASA", "AS", "ALASKA", LG_ALASKA, 0x00C0C0}, {"QXE", "QX", "HORIZON", LG_ALASKA, 0x00C0C0},
  {"JBU", "B6", "JETBLUE", LG_JETBLUE, 0x4080FF}, {"FDX", "FX", "FEDEX", LG_FEDEX, 0xA060FF},
  {"UPS", "5X", "UPS", LG_UPS, 0xFFB000}, {"NKS", "NK", "SPIRIT", LG_SPIRIT, 0xFFE000},
  {"SKW", "OO", "SKYWEST", LG_NONE, 0x80B0FF}, {"RPA", "YX", "REPUBLIC", LG_NONE, 0x80B0FF},
  {"CPZ", "CP", "COMPASS", LG_NONE, 0x80B0FF}, {"GJS", "G7", "GOJET", LG_NONE, 0x80B0FF},
  {"ASH", "YV", "MESA", LG_NONE, 0x80B0FF}, {"FFT", "F9", "FRONTIER", LG_NONE, 0x30D060},
  {"AAY", "G4", "ALLEGIANT", LG_NONE, 0xFF8000}, {"HAL", "HA", "HAWAIIAN", LG_NONE, 0xC060FF},
  {"SCX", "SY", "SUN COUNTRY", LG_NONE, 0xFF9020}, {"MXY", "MX", "BREEZE", LG_NONE, 0x60A0FF},
  {"GTI", "5Y", "ATLAS AIR", LG_NONE, 0xC0C8D0}, {"ABX", "GB", "ABX AIR", LG_NONE, 0xC0C8D0},
  {"ATN", "8C", "AIR TRANSPORT", LG_NONE, 0xC0C8D0}, {"CKS", "K4", "KALITTA", LG_NONE, 0xC0C8D0},
  {"SWQ", "WQ", "SWIFT AIR", LG_NONE, 0xC0C8D0}, {"PAC", "PO", "POLAR", LG_NONE, 0xC0C8D0},
  {"AMX", "AM", "AEROMEXICO", LG_NONE, 0x4080FF}, {"VOI", "Y4", "VOLARIS", LG_NONE, 0xC040FF},
  {"VIV", "VB", "VIVA AEROBUS", LG_NONE, 0x30D060}, {"ACA", "AC", "AIR CANADA", LG_NONE, 0xFF3030},
  {"WJA", "WS", "WESTJET", LG_NONE, 0x30C0C0}, {"FLE", "F8", "FLAIR", LG_NONE, 0x60FF60},
  {"EVA", "BR", "EVA AIR", LG_NONE, 0x30D060}, {"CPA", "CX", "CATHAY", LG_NONE, 0x30C0A0},
  {"KAL", "KE", "KOREAN AIR", LG_NONE, 0x60B0FF}, {"AAR", "OZ", "ASIANA", LG_NONE, 0xC0C8D0},
  {"JAL", "JL", "JAPAN AIRLINES", LG_NONE, 0xFF3030}, {"ANA", "NH", "ANA", LG_NONE, 0x4080FF},
  {"CAL", "CI", "CHINA AIRLINES", LG_NONE, 0xFF80C0}, {"SIA", "SQ", "SINGAPORE", LG_NONE, 0xFFB000},
  {"QFA", "QF", "QANTAS", LG_NONE, 0xFF3030}, {"ANZ", "NZ", "AIR NEW ZEALAND", LG_NONE, 0xC0C8D0},
  {"BAW", "BA", "BRITISH AIRWAYS", LG_NONE, 0x4080FF}, {"VIR", "VS", "VIRGIN ATLANTIC", LG_NONE, 0xFF3030},
  {"AFR", "AF", "AIR FRANCE", LG_NONE, 0x4080FF}, {"KLM", "KL", "KLM", LG_NONE, 0x30B0FF},
  {"DLH", "LH", "LUFTHANSA", LG_NONE, 0xFFC000}, {"UAE", "EK", "EMIRATES", LG_NONE, 0xFF3030},
  {"QTR", "QR", "QATAR", LG_NONE, 0xC060A0}, {"THY", "TK", "TURKISH", LG_NONE, 0xFF3030},
  {"CES", "MU", "CHINA EASTERN", LG_NONE, 0x4080FF}, {"CSN", "CZ", "CHINA SOUTHERN", LG_NONE, 0x60B0FF},
  {"CCA", "CA", "AIR CHINA", LG_NONE, 0xFF3030}, {"PAL", "PR", "PHILIPPINE", LG_NONE, 0x4080FF},
  {"EJA", "", "NETJETS", LG_NONE, 0xC0C8D0}, {"LXJ", "", "FLEXJET", LG_NONE, 0xC0C8D0},
  {"VJT", "", "VISTAJET", LG_NONE, 0xFF3030}, {"JSX", "XE", "JSX", LG_NONE, 0xC0C8D0},
  {"KAP", "9K", "CAPE AIR", LG_NONE, 0xC0C8D0}, {"AIP", "", "ALPINE AIR", LG_NONE, 0xC0C8D0},
  {"RCH", "", "US AIR FORCE", LG_NONE, 0x80B0FF}, {"CAP", "", "CIVIL AIR PATROL", LG_NONE, 0xFF3030},
};

// Airline callsigns are three letters then the flight number ("SWA3091").
static bool airlineCallsign(const char *cs) {
  return isalpha(cs[0]) && isalpha(cs[1]) && isalpha(cs[2]) && isdigit(cs[3]);
}

static const Airline *airlineOf(const char *cs) {
  if (!airlineCallsign(cs)) return nullptr;
  for (const Airline &a : AIRLINES) if (!strncmp(a.icao, cs, 3)) return &a;
  return nullptr;
}

// ---- Geometry -----------------------------------------------------------------
static float milesBetween(float lat1, float lon1, float lat2, float lon2) {
  float p1 = radians(lat1), p2 = radians(lat2), dp = p2 - p1, dl = radians(lon2 - lon1);
  float a = sinf(dp / 2) * sinf(dp / 2) + cosf(p1) * cosf(p2) * sinf(dl / 2) * sinf(dl / 2);
  return 3958.8f * 2 * atan2f(sqrtf(a), sqrtf(1 - a));
}

static float bearingTo(float lat1, float lon1, float lat2, float lon2) {
  float p1 = radians(lat1), p2 = radians(lat2), dl = radians(lon2 - lon1);
  float b = degrees(atan2f(sinf(dl) * cosf(p2), cosf(p1) * sinf(p2) - sinf(p1) * cosf(p2) * cosf(dl)));
  return fmodf(b + 360, 360);
}

// ---- Fetching (core 0) --------------------------------------------------------
// The airline and route for one callsign, kept until the nearest plane changes.
struct RouteInfo {
  char callsign[9] = "";
  char airline[28] = "";
  char iata[3] = "";
  char from[4] = "", to[4] = "";
  float fromLat = 0, fromLon = 0, toLat = 0, toLon = 0;
  bool known = false;
};

static void lookupRoute(RouteInfo &r, const char *callsign) {
  r = RouteInfo();
  strlcpy(r.callsign, callsign, sizeof(r.callsign));
  if (!airlineCallsign(callsign)) return;          // private planes: adsbdb only knows airlines
  static JsonDocument filter;
  if (filter.isNull()) {
    JsonObject fr = filter["response"]["flightroute"].to<JsonObject>();
    fr["airline"]["name"] = true;
    fr["airline"]["iata"] = true;
    fr["origin"]["iata_code"] = true;
    fr["origin"]["latitude"] = true;
    fr["origin"]["longitude"] = true;
    fr["destination"]["iata_code"] = true;
    fr["destination"]["latitude"] = true;
    fr["destination"]["longitude"] = true;
  }
  JsonDocument doc;
  if (!fetchJson(String("https://api.adsbdb.com/v0/callsign/") + callsign, doc, filter)) return;
  JsonObject fr = doc["response"]["flightroute"];
  if (fr.isNull()) return;
  String name = fr["airline"]["name"] | "";
  name.toUpperCase();
  strlcpy(r.airline, name.c_str(), sizeof(r.airline));
  strlcpy(r.iata, fr["airline"]["iata"] | "", sizeof(r.iata));
  strlcpy(r.from, fr["origin"]["iata_code"] | "", sizeof(r.from));
  strlcpy(r.to, fr["destination"]["iata_code"] | "", sizeof(r.to));
  r.fromLat = fr["origin"]["latitude"] | 0.0f;
  r.fromLon = fr["origin"]["longitude"] | 0.0f;
  r.toLat = fr["destination"]["latitude"] | 0.0f;
  r.toLon = fr["destination"]["longitude"] | 0.0f;
  r.known = r.from[0] && r.to[0];
}

static bool fetchAircraft(float lat, float lon, JsonDocument &doc, JsonArray &list) {
  static JsonDocument filter;
  if (filter.isNull()) {
    for (const char *key : {"aircraft", "ac"}) {   // adsb.fi, adsb.lol
      JsonObject a = filter[key].add<JsonObject>();
      for (const char *f : {"hex", "flight", "r", "t", "desc", "alt_baro", "gs", "track", "lat", "lon", "seen_pos"}) a[f] = true;
    }
  }
  char url[120];
  snprintf(url, sizeof(url), "https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d", lat, lon, FLIGHT_RADIUS_NM);
  if (fetchJson(url, doc, filter)) { list = doc["aircraft"]; return true; }
  doc.clear();
  snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", lat, lon, FLIGHT_RADIUS_NM);
  if (fetchJson(url, doc, filter)) { list = doc["ac"]; return true; }
  return false;
}

static void flightsTask(void *) {
  static RouteInfo route;
  for (;;) {
    NOTE_STACK(3);
    // Only while the mode is on screen: no point using the APIs otherwise.
    if (millis() - flightsSeenAt > 3000 || WiFi.status() != WL_CONNECTED) {
      xSemaphoreTake(skyLock, portMAX_DELAY);
      sky.fetched = false;                      // stale by the time the mode comes back
      xSemaphoreGive(skyLock);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    uint32_t started = millis();
    Ambient home = ambient();
    JsonDocument doc;
    JsonArray list;
    if (fetchAircraft(home.lat, home.lon, doc, list)) {
      Flight best;
      float bestLat = 0, bestLon = 0;              // where it is, for the route check below
      uint8_t count = 0;
      for (JsonObject a : list) {
        if (!a["alt_baro"].is<int>() || !a["lat"].is<float>()) continue;   // "ground", or no position
        if ((a["seen_pos"] | 0.0f) > 60) continue;                         // position gone stale
        count++;
        float miles = milesBetween(home.lat, home.lon, a["lat"], a["lon"]);
        if (best.valid && miles >= best.miles) continue;
        best = Flight();
        best.valid = true;
        best.miles = miles;
        strlcpy(best.hex, a["hex"] | "", sizeof(best.hex));
        String cs = a["flight"] | "";
        cs.trim();
        if (!cs.length()) cs = a["r"] | "";      // no callsign: the registration
        if (!cs.length()) cs = best.hex;
        cs.toUpperCase();
        strlcpy(best.callsign, cs.c_str(), sizeof(best.callsign));
        const char *type = a["desc"] | (a["t"] | "");
        strlcpy(best.type, type, sizeof(best.type));
        best.altFt = a["alt_baro"];
        best.mph = lroundf((a["gs"] | 0.0f) * 1.15078f);
        best.track = a["track"] | 0.0f;
        bestLat = a["lat"];
        bestLon = a["lon"];
      }
      if (best.valid) {
        if (strcmp(route.callsign, best.callsign)) lookupRoute(route, best.callsign);
        strlcpy(best.airline, route.airline, sizeof(best.airline));
        const Airline *known = airlineOf(best.callsign);
        strlcpy(best.iata, known ? known->iata : route.iata, sizeof(best.iata));
        if (route.known) {
          float off = fabsf(fmodf(bearingTo(bestLat, bestLon, route.toLat, route.toLon) - best.track + 540, 360) - 180);
          float toGo = milesBetween(bestLat, bestLon, route.toLat, route.toLon);
          bool landing = toGo < 15;                    // circling in: any heading
          // And on the way: a stale route can point the right way from the wrong state.
          float direct = milesBetween(route.fromLat, route.fromLon, route.toLat, route.toLon);
          bool onTheWay = milesBetween(route.fromLat, route.fromLon, bestLat, bestLon) + toGo < direct * 1.3f + 30;
          if (onTheWay && (off < 70 || landing)) snprintf(best.route, sizeof(best.route), "%s>%s", route.from, route.to);
        }
      }
      xSemaphoreTake(skyLock, portMAX_DELAY);
      sky.fetched = true;
      sky.count = count;
      sky.nearest = best;
      xSemaphoreGive(skyLock);
    }
    while (millis() - started < FLIGHT_POLL_MS) vTaskDelay(pdMS_TO_TICKS(250));
  }
}

static void setupFlights() {
  skyLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(flightsTask, "flights", 10240, nullptr, 0, nullptr, 0);   // idle priority: see setupGames()
}

// ---- Drawing --------------------------------------------------------------------
static void drawLogo(const Logo &l, int x0) {
  for (int i = 0; i < 256; i++) {
    const char *k = strchr(l.keys, l.px[i]);
    if (l.px[i] != '.' && k) px(x0 + i % 16, i / 16) = colorOf(l.colors[k - l.keys]);
  }
}

// No logo: the two-letter code, white on a dim wash of the airline's colour.
static void drawCodeCard(const char *code, CRGB c, int x0) {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < PANEL; x++) px(x0 + x, y) = CRGB(c).nscale8(40);
  drawText(frame, code, x0 + 2, 4, 1, CRGB(230, 230, 230));
  for (int x = 2; x < 13; x++) px(x0 + x, 12) = c;
}

// A plane, nose up, turned to `track` degrees (0 = north = up).
static void drawPlane(float track, int x0, int y0, CRGB c) {
  static const char *PLANE =
    "...##..."
    "...##..."
    "..####.."
    ".######."
    "########"
    "...##..."
    "..####.."
    "........";
  float s = sinf(radians(track)), co = cosf(radians(track));
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      float dx = x - 3.5f, dy = y - 3.5f;
      int sx = lroundf(3.5f + co * dx + s * dy), sy = lroundf(3.5f - s * dx + co * dy);
      if (sx >= 0 && sx < 8 && sy >= 0 && sy < 8 && PLANE[sy * 8 + sx] == '#') px(x0 + x, y0 + y) = c;
    }
}

static String withCommas(int n) {
  String s = String(n);
  for (int i = s.length() - 3; i > 0; i -= 3) s = s.substring(0, i) + "," + s.substring(i);
  return s;
}

// The line that scrolls along the bottom.
static String flightLine(const FlightSky &s) {
  if (WiFi.status() != WL_CONNECTED) return "NO WIFI";
  if (!s.fetched) return "LOOKING UP";
  if (!s.nearest.valid) return "CLEAR SKIES";
  const Flight &f = s.nearest;
  const Airline *a = airlineOf(f.callsign);
  String line;
  if (a) line = String(a->name) + " " + (f.callsign + 3);
  else if (f.airline[0]) line = String(f.airline) + " " + (f.callsign + 3);
  else line = f.callsign;
  if (f.route[0]) line += String("  ") + f.route;
  if (f.type[0]) line += String("  ") + f.type;
  line += "  " + withCommas(f.altFt) + "FT  " + String(f.miles, 1) + "MI  " + String(f.mph) + "MPH";
  line.toUpperCase();
  return line;
}

// Test: a made-up flight passing overhead for 40 s (/api/flight/test).
constexpr uint32_t FLIGHT_DEMO_MS = 40000;
static volatile uint32_t flightDemoStart = 0;   // 0 = off
static volatile bool flightRestart = false;     // drop the current pass
static Flight flightDemo;                       // callsign, route and type to show

static void startFlightDemo(const String &callsign, const String &route, const String &type) {
  flightDemo = Flight();
  strlcpy(flightDemo.callsign, callsign.c_str(), sizeof(flightDemo.callsign));
  strlcpy(flightDemo.route, route.c_str(), sizeof(flightDemo.route));
  strlcpy(flightDemo.type, type.c_str(), sizeof(flightDemo.type));
  const Airline *a = airlineOf(flightDemo.callsign);
  if (a) strlcpy(flightDemo.iata, a->iata, sizeof(flightDemo.iata));
  flightDemoStart = millis() | 1;
  flightRestart = true;
}

static FlightSky demoSky(uint32_t now) {
  FlightSky s;
  s.fetched = true;
  s.count = 1;
  Flight &f = s.nearest = flightDemo;
  float t = (now - flightDemoStart) / (float)FLIGHT_DEMO_MS;   // 0..1, overhead at 0.5
  f.valid = true;
  strcpy(f.hex, "demo01");
  f.altFt = 9000 - (int)(t * 3000);              // descending into Reno
  f.mph = 290;
  f.miles = fabsf(t - 0.5f) * 24 + 0.3f;         // 12 mi out, overhead, 12 mi past
  f.track = 340;
  return s;
}

constexpr uint32_t FLIGHT_CARD_MS = 3000;

static bool hasCard(const Flight &f) {
  const Airline *a = airlineOf(f.callsign);
  return f.valid && ((a && a->logo) || f.iata[0]);
}

static void fxFlights() {
  flightsSeenAt = millis();
  fill_solid(frame, NUM_LEDS, CRGB::Black);
  bool demo = flightDemoStart && millis() - flightDemoStart < FLIGHT_DEMO_MS;
  if (flightDemoStart && !demo) { flightDemoStart = 0; flightRestart = true; }
  FlightSky s = demo ? demoSky(millis()) : currentSky();

  // A round (card, then one pass of the details) runs to the end before the
  // next plane takes over.
  static Flight shown;
  static String line;
  static uint32_t roundStart = 0, cardMs = 0;
  uint32_t now = millis();
  int offset = 0;
  bool newRound = flightRestart || !line.length();
  if (!newRound && now - roundStart >= cardMs) {
    offset = (now - roundStart - cardMs) / 55;
    newRound = offset >= textWidth(line, 1) + W;
  }
  if (newRound) {
    flightRestart = false;
    shown = s.nearest;
    line = flightLine(s);
    roundStart = now;
    cardMs = hasCard(shown) ? FLIGHT_CARD_MS : 0;
    offset = 0;
  }
  if (s.nearest.valid && !strcmp(s.nearest.hex, shown.hex)) shown = s.nearest;   // same plane: live numbers
  else if (!s.nearest.valid) shown.valid = false;                                 // gone: don't show a ghost

  const Airline *a = airlineOf(shown.callsign);
  CRGB color = a ? colorOf(a->color) : CRGB(200, 200, 200);
  int ox = (W - PANEL) / 2;   // on a wide grid, keep the picture centred
  if (now - roundStart < cardMs) {
    if (a && a->logo) drawLogo(LOGOS[a->logo], ox);
    else drawCodeCard(shown.iata, color, ox);
    return;
  }
  if (!shown.valid) {
    drawPlane(now / 20 % 360, ox + 4, 0, CRGB(25, 25, 40));   // idle: a dim plane circling
    color = CRGB(60, 80, 120);
  } else {
    drawPlane(shown.track, ox, 0, color);
    int thousands = (shown.altFt + 500) / 1000;
    String alt = String(min(thousands, 99));
    CRGB altColor = CHSV(20 + min(shown.altFt, 40000) * 130 / 40000, 255, 255);
    drawDigits(frame, alt, ox + 16 - digitsWidth(alt), 1, altColor);
    // Under the digits, a bar for how close it is: full at 0, empty at the edge.
    int bar = constrain(lroundf(7 * (1 - shown.miles / (FLIGHT_RADIUS_NM * 1.15078f))), 1, 7);
    for (int x = 0; x < bar; x++) px(ox + 9 + x, 7) = CRGB(0, 90, 40);
  }
  drawText(frame, line, W - offset, 9, 1, color);
}

static String flightsJson() {
  FlightSky s = currentSky();
  if (!s.nearest.valid) return "{\"count\":" + String(s.count) + "}";
  const Flight &f = s.nearest;
  return "{\"count\":" + String(s.count) + ",\"callsign\":\"" + jsonEscape(f.callsign) + "\",\"route\":\"" + f.route +
         "\",\"type\":\"" + jsonEscape(f.type) + "\",\"alt\":" + String(f.altFt) + ",\"miles\":" + String(f.miles, 1) +
         ",\"mph\":" + String(f.mph) + "}";
}
