// Home Assistant over MQTT. Included once, from main.cpp, after schedule.h.
//
// With a broker set on the page (Setup, Home Assistant: HA's Mosquitto
// add-on), the grid announces itself by MQTT discovery and appears in HA as
// one device:
//   light       on/off, brightness, colour, and the effects plus "Auto-cycle"
//   select      palette
//   switches    schedule, auto-cycle, game screen, quarter-hour card
//   number      speed
//   text        message (scrolls it, in Text mode)
//   button      show the quarter-hour card now
//   sensors     next scheduled change, Wi-Fi signal, IP address
// The device page links back to this page at the grid's current IP, so HA
// keeps the address even when the router hands out a new one.
//
// Runs in its own task on core 0. Every entity reads one retained state
// message, sent whenever a setting changes and once a minute anyway.
#pragma once
#include <PubSubClient.h>

static WiFiClient mqttNet;
static PubSubClient mqtt(mqttNet);
static String mqttId, mqttBase, mqttName;   // "ledgrid_a1b2c3", "ledgrid/a1b2c3", "LED Grid b2c3"
static volatile bool mqttReconfig = true;   // broker settings changed: reconnect
static volatile bool mqttAnnounce = false;  // HA restarted: send discovery again
// 0 off, 1 waiting for Wi-Fi, 2 connecting, 3 connected; below 0 (or 4, 5) is PubSubClient's last error
static volatile int mqttStatus = 0;

static const char *AUTO_EFFECT = "Auto-cycle";
static bool hiddenEffect(uint8_t m) { return m == M_LAYOUT; }

static String mqttDevice() {
  String ip = WiFi.localIP().toString();
  return "{\"ids\":[\"" + mqttId + "\"],\"name\":\"" + mqttName + "\",\"mf\":\"DIY\",\"mdl\":\"ESP32 + " +
         String(W) + "x" + String(H) + " WS2812B\",\"cu\":\"http://" + ip + "/\"}";
}

static void announce(const char *component, const char *obj, const String &body) {
  String topic = String("homeassistant/") + component + "/" + mqttId + "/" + obj + "/config";
  String p = "{\"~\":\"" + mqttBase + "\",\"uniq_id\":\"" + mqttId + "_" + obj +
             "\",\"avty_t\":\"~/status\",\"stat_t\":\"~/state\",\"dev\":" + mqttDevice() + "," + body + "}";
  if (!mqtt.publish(topic.c_str(), p.c_str(), true)) Serial.printf("MQTT: discovery for %s too big?\n", obj);
}

static void announceSwitch(const char *obj, const char *name, const char *icon) {
  announce("switch", obj, String("\"name\":\"") + name + "\",\"cmd_t\":\"~/" + obj + "/set\",\"val_tpl\":\"{{ value_json." + obj +
           " }}\",\"icon\":\"" + icon + "\"");
}

static void announceAll() {
  String fx = String("[\"") + AUTO_EFFECT + "\"";
  for (int i = 0; i < M_COUNT; i++) if (!hiddenEffect(i)) fx += ",\"" + String(MODE_NAMES[i]) + "\"";
  announce("light", "light", "\"name\":null,\"schema\":\"json\",\"cmd_t\":\"~/light/set\",\"brightness\":true,"
           "\"sup_clrm\":[\"rgb\"],\"effect\":true,\"fx_list\":" + fx + "]");
  String pals = "[";
  for (int i = 0; i < PALETTE_COUNT; i++) pals += String(i ? "," : "") + "\"" + PALETTE_NAMES[i] + "\"";
  announce("select", "palette", "\"name\":\"Palette\",\"cmd_t\":\"~/palette/set\",\"val_tpl\":\"{{ value_json.palette }}\","
           "\"options\":" + pals + "],\"icon\":\"mdi:palette\"");
  announceSwitch("schedule", "Schedule", "mdi:calendar-clock");
  announceSwitch("autocycle", "Auto-cycle", "mdi:shuffle-variant");
  announceSwitch("gamescreen", "Game screen", "mdi:scoreboard");
  announceSwitch("card", "Quarter-hour card", "mdi:clock-outline");
  announce("number", "speed", "\"name\":\"Speed\",\"cmd_t\":\"~/speed/set\",\"val_tpl\":\"{{ value_json.speed }}\","
           "\"min\":1,\"max\":100,\"mode\":\"slider\",\"icon\":\"mdi:speedometer\"");
  announce("text", "text", "\"name\":\"Message\",\"cmd_t\":\"~/text/set\",\"val_tpl\":\"{{ value_json.text }}\","
           "\"max\":120,\"icon\":\"mdi:message-text\"");
  announce("button", "showcard", "\"name\":\"Show clock card\",\"cmd_t\":\"~/showcard/press\",\"icon\":\"mdi:clock-fast\"");
  announce("sensor", "next", "\"name\":\"Next change\",\"val_tpl\":\"{{ value_json.next }}\",\"icon\":\"mdi:timer-outline\"");
  announce("sensor", "rssi", "\"name\":\"Wi-Fi signal\",\"val_tpl\":\"{{ value_json.rssi }}\",\"unit_of_meas\":\"dBm\","
           "\"dev_cla\":\"signal_strength\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\"");
  announce("sensor", "ip", "\"name\":\"IP address\",\"val_tpl\":\"{{ value_json.ip }}\",\"ent_cat\":\"diagnostic\",\"icon\":\"mdi:ip-network\"");
}

static String mqttState() {
  JsonDocument d;
  d["state"] = S.power ? "ON" : "OFF";
  d["brightness"] = S.brightness;
  d["color_mode"] = "rgb";
  d["color"]["r"] = (S.color >> 16) & 0xFF;
  d["color"]["g"] = (S.color >> 8) & 0xFF;
  d["color"]["b"] = S.color & 0xFF;
  d["effect"] = S.autoCycle ? AUTO_EFFECT : MODE_NAMES[S.mode];
  d["palette"] = PALETTE_NAMES[S.palette];
  d["schedule"] = S.schedOn ? "ON" : "OFF";
  d["autocycle"] = S.autoCycle ? "ON" : "OFF";
  d["gamescreen"] = S.gamePong ? "ON" : "OFF";
  d["card"] = S.clockCard ? "ON" : "OFF";
  d["speed"] = S.speed;
  d["text"] = S.text;
  String next = schedNext();
  d["next"] = next.length() ? next : S.schedOn ? "Waiting for the clock" : "No schedule";
  d["rssi"] = WiFi.RSSI();
  d["ip"] = WiFi.localIP().toString();
  String out;
  serializeJson(d, out);
  return out;
}

static void pickMode(uint8_t m) {
  if (m != S.mode) { S.mode = m; fill_solid(frame, NUM_LEDS, CRGB::Black); }
  S.autoCycle = false;
}

static void onMqtt(char *topicC, byte *payload, unsigned len) {
  String topic = topicC, msg;
  msg.reserve(len);
  for (unsigned i = 0; i < len; i++) msg += (char)payload[i];
  if (topic == "homeassistant/status") { if (msg == "online") mqttAnnounce = true; return; }
  if (!topic.startsWith(mqttBase + "/")) return;
  String cmd = topic.substring(mqttBase.length() + 1);
  bool on = msg == "ON";

  if (cmd == "light/set") {
    JsonDocument d;
    if (deserializeJson(d, msg)) return;
    const char *state = d["state"] | "";
    if (!strcmp(state, "ON")) setPower(true);
    else if (!strcmp(state, "OFF")) setPower(false);
    if (d["brightness"].is<int>()) S.brightness = constrain(d["brightness"].as<int>(), 1, 255);
    const char *fx = d["effect"] | "";
    if (*fx) {
      if (!strcmp(fx, AUTO_EFFECT)) S.autoCycle = true;
      else for (int i = 0; i < M_COUNT; i++) if (!strcmp(fx, MODE_NAMES[i])) pickMode(i);
    }
    if (d["color"].is<JsonObject>()) {
      S.color = (uint32_t)(d["color"]["r"] | 0) << 16 | (uint32_t)(d["color"]["g"] | 0) << 8 | (uint32_t)(d["color"]["b"] | 0);
      // A colour picked on its own means "this colour", unless the effect already uses it.
      bool usesColor = S.mode == M_SOLID || S.mode == M_TEXT || S.mode == M_RADAR || S.mode == M_MATRIX;
      if (!*fx && (S.autoCycle || !usesColor)) pickMode(M_SOLID);
    }
  } else if (cmd == "palette/set") {
    for (int i = 0; i < PALETTE_COUNT; i++) if (msg == PALETTE_NAMES[i]) S.palette = i;
  } else if (cmd == "schedule/set") {
    setSchedule(on);
  } else if (cmd == "autocycle/set") {
    S.autoCycle = on;
  } else if (cmd == "gamescreen/set") {
    S.gamePong = on;
  } else if (cmd == "card/set") {
    S.clockCard = on;
  } else if (cmd == "speed/set") {
    S.speed = constrain(msg.toInt(), 1, 100);
  } else if (cmd == "text/set") {
    String t;
    for (char c : msg) if (c >= 32 && c < 127 && t.length() < 120) t += c;
    S.text = t;
    if (t.length()) { pickMode(M_TEXT); setPower(true); }
    else if (S.mode == M_TEXT) S.autoCycle = true;   // message cleared: back to the animations
  } else if (cmd == "showcard/press") {
    triggerCard();
    return;
  } else {
    return;
  }
  markDirty();
}

static void mqttTask(void *) {
  static char host[65], user[65], pass[65];
  uint16_t port = 1883;
  uint32_t sent = 0, lastSend = 0, lastTry = 0;
  bool announced = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(50));
    NOTE_STACK(4);
    if (mqttReconfig) {
      mqttReconfig = false;
      if (mqtt.connected()) { mqtt.publish((mqttBase + "/status").c_str(), "offline", true); mqtt.disconnect(); }
      strlcpy(host, S.mqttHost.c_str(), sizeof(host));
      strlcpy(user, S.mqttUser.c_str(), sizeof(user));
      strlcpy(pass, S.mqttPass.c_str(), sizeof(pass));
      port = S.mqttPort;
      lastTry = 0;
    }
    if (!*host) { mqttStatus = 0; continue; }
    if (WiFi.status() != WL_CONNECTED) { mqttStatus = 1; continue; }
    if (!mqtt.connected()) {
      if (lastTry && millis() - lastTry < 15000) continue;
      lastTry = millis();
      mqttStatus = 2;
      mqtt.setServer(host, port);
      String will = mqttBase + "/status";
      if (!mqtt.connect(mqttId.c_str(), *user ? user : nullptr, *user ? pass : nullptr, will.c_str(), 0, true, "offline")) {
        mqttStatus = mqtt.state() ? mqtt.state() : -2;
        Serial.printf("MQTT: can't connect to %s:%u (%d)\n", host, port, mqtt.state());
        continue;
      }
      mqtt.publish(will.c_str(), "online", true);
      mqtt.subscribe((mqttBase + "/+/set").c_str());
      mqtt.subscribe((mqttBase + "/showcard/press").c_str());
      mqtt.subscribe("homeassistant/status");
      announced = false;
      mqttStatus = 3;
      Serial.printf("MQTT: connected to %s as %s\n", host, mqttId.c_str());
    }
    if (!announced || mqttAnnounce) { mqttAnnounce = false; announceAll(); announced = true; sent = 0; }
    mqtt.loop();
    if (sent != stateVersion || millis() - lastSend > 60000) {
      sent = stateVersion;
      lastSend = millis();
      mqtt.publish((mqttBase + "/state").c_str(), mqttState().c_str(), true);
    }
  }
}

// The task and its buffer cost ~7 KB that the HTTPS fetches need, so they
// only exist once a broker is set.
static void startMqtt() {
  static bool started = false;
  if (started || !S.mqttHost.length()) return;
  started = true;
  mqtt.setBufferSize(1280);
  xTaskCreatePinnedToCore(mqttTask, "mqtt", 5120, nullptr, 0, nullptr, 0);   // idle priority, like the fetch tasks
}

static void setupMqtt() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char id[16];
  snprintf(id, sizeof(id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  mqttId = String("ledgrid_") + id;
  mqttBase = String("ledgrid/") + id;
  mqttName = String("LED Grid ") + (id + 2);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(4);
  mqtt.setCallback(onMqtt);
  startMqtt();
}

static String mqttJson() {
  return "{\"id\":\"" + mqttId + "\",\"host\":\"" + jsonEscape(S.mqttHost) + "\",\"port\":" + String(S.mqttPort) +
         ",\"user\":\"" + jsonEscape(S.mqttUser) + "\",\"hasPass\":" + (S.mqttPass.length() ? "true" : "false") +
         ",\"status\":" + String(mqttStatus) + "}";
}
