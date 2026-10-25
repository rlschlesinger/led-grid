// On/off schedule. Included once, from main.cpp, after ambient.h.
//
// Weekdays and weekends each get an on time and an off time. Either can be a
// clock time or sunrise/sunset give or take some minutes. An off time earlier
// than the on time runs past midnight. Saturday and Sunday use the weekend
// rule, judged by the day the on time falls on.
//
// The schedule only acts when it changes its mind, so switching the grid on or
// off by hand (page or Home Assistant) holds until the next scheduled change.
// While off the panel is dark, but a followed team's shows still play.
//
// It needs the clock, plus the UTC offset and sun times from the weather
// fetch. The last ones fetched are kept in flash, so a weather outage after a
// restart doesn't stop it; with no clock at all it leaves the power alone.
#pragma once

enum SchedKind : uint8_t { SK_TIME, SK_SUNRISE, SK_SUNSET };
struct SchedEv { SchedKind kind; int16_t val; };   // minute of the day, or minutes from the sun event
static SchedEv sched[4];          // weekday on, weekday off, weekend on, weekend off
static int8_t schedPrev = -1;     // what the schedule last wanted; -1 = apply at the next check

// "t:420,t:1380,ss:-30,t:0": each event is t:<minute of day>, sr:<offset> or ss:<offset>.
static bool parseSched(const String &s, SchedEv out[4]) {
  int from = 0;
  for (int i = 0; i < 4; i++) {
    int comma = s.indexOf(',', from);
    String part = s.substring(from, comma < 0 ? s.length() : comma);
    int colon = part.indexOf(':');
    if (colon < 0) return false;
    String k = part.substring(0, colon);
    int v = part.substring(colon + 1).toInt();
    if (k == "t" && v >= 0 && v < 1440) out[i] = {SK_TIME, (int16_t)v};
    else if ((k == "sr" || k == "ss") && v >= -240 && v <= 240) out[i] = {k == "sr" ? SK_SUNRISE : SK_SUNSET, (int16_t)v};
    else return false;
    if (comma < 0) return i == 3;
    from = comma + 1;
  }
  return true;
}

struct SchedClock { bool ok; time_t local; int sunrise, sunset; };   // sun times in minutes of the local day
static int32_t savedOffset = 0;
static int16_t savedRise = -1, savedSet = -1;   // from flash; -1 = never fetched

static SchedClock schedClock() {
  Ambient a = ambient();
  time_t now = time(nullptr);
  if (now < 1700000000) return {false, 0, 0, 0};
  if (!a.weather) {
    if (savedRise < 0) return {false, 0, 0, 0};
    return {true, now + savedOffset, savedRise, savedSet};
  }
  auto minute = [&](time_t t) { return (int)(((t + a.utcOffset) % 86400 + 86400) % 86400 / 60); };
  return {true, now + a.utcOffset, minute(a.sunrise), minute(a.sunset)};
}

static void loadSchedClock() {
  Preferences p;
  p.begin("sched", true);
  savedOffset = p.getInt("off", 0);
  savedRise = p.getShort("rise", -1);
  savedSet = p.getShort("set", -1);
  p.end();
}

// Sun times drift a minute or two a day, so only write when they've moved a bit.
static void keepSchedClock(const SchedClock &c) {
  Ambient a = ambient();
  if (!a.weather) return;
  if (a.utcOffset == savedOffset && abs(c.sunrise - savedRise) < 5 && abs(c.sunset - savedSet) < 5) return;
  savedOffset = a.utcOffset;
  savedRise = c.sunrise;
  savedSet = c.sunset;
  Preferences p;
  p.begin("sched", false);
  p.putInt("off", savedOffset);
  p.putShort("rise", savedRise);
  p.putShort("set", savedSet);
  p.end();
}

static int evMinute(const SchedEv &e, const SchedClock &c) {
  int m = e.kind == SK_TIME ? e.val : (e.kind == SK_SUNRISE ? c.sunrise : c.sunset) + e.val;
  return (m % 1440 + 1440) % 1440;
}

// Whether the schedule wants the grid on at local time t.
static bool schedWants(time_t t, const SchedClock &c) {
  int m = t % 86400 / 60;
  int dow = (t / 86400 + 4) % 7;       // 1 Jan 1970 was a Thursday; 0 = Sunday
  for (int back = 0; back < 2; back++) {   // today's window, then yesterday's running past midnight
    int d = (dow + 7 - back) % 7;
    const SchedEv *r = sched + (d == 0 || d == 6 ? 2 : 0);
    int on = evMinute(r[0], c), off = evMinute(r[1], c);
    if (on == off) continue;
    bool in = back == 0 ? (on < off ? m >= on && m < off : m >= on) : on > off && m < off;
    if (in) return true;
  }
  return false;
}

// "Off at 11:00 PM", "On tomorrow at 7:00 AM", or "" if the schedule can't run.
static String schedNext() {
  SchedClock c = schedClock();
  if (!S.schedOn || !c.ok) return "";
  bool now = schedWants(c.local, c);
  time_t start = c.local - c.local % 60;
  for (int k = 1; k <= 8 * 1440; k++) {
    time_t t = start + k * 60;
    if (schedWants(t, c) == now) continue;
    int m = t % 86400 / 60, h = m / 60 % 12;
    long days = t / 86400 - c.local / 86400;
    static const char *DAYS[] = {" Sun", " Mon", " Tue", " Wed", " Thu", " Fri", " Sat"};
    char buf[40];
    snprintf(buf, sizeof(buf), "%s%s at %d:%02d %s", now ? "Off" : "On",
             days == 0 ? "" : days == 1 ? " tomorrow" : DAYS[(t / 86400 + 4) % 7], h ? h : 12, m % 60, m < 720 ? "AM" : "PM");
    return buf;
  }
  return now ? "Always on" : "Always off";
}

static void setSchedule(bool on) {
  if (on && !S.schedOn) schedPrev = -1;   // take effect straight away
  S.schedOn = on;
}

static void setPower(bool on) { S.power = on; }

// Called from loop(): at most once a second, act on a scheduled change.
static void schedTick() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  if (!S.schedOn) return;
  static bool loaded = false;
  if (!loaded) { loadSchedClock(); loaded = true; }
  SchedClock c = schedClock();
  if (!c.ok) return;
  keepSchedClock(c);
  bool want = schedWants(c.local, c);
  if (want == schedPrev) return;
  schedPrev = want;
  S.power = want;
  markDirty();
}
