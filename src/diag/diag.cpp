// Diagnostic firmware: `pio run -e diag -t upload`.
// Sends the same signal on eight left-side pins, so the panel lights whichever
// one the data wire is on. The whole panel cycles red, green, blue, white at
// full brightness (power-capped at 500 mA for USB). The blue onboard LED
// (GPIO 2) blinks so you can see the firmware is running.
#include <Arduino.h>
#include <FastLED.h>

constexpr uint16_t NUM_LEDS = 256;
CRGB leds[NUM_LEDS];

void setup() {
  Serial.begin(115200);
  pinMode(2, OUTPUT);
  FastLED.addLeds<WS2812B, 13, GRB>(leds, NUM_LEDS);
  FastLED.addLeds<WS2812B, 12, GRB>(leds, NUM_LEDS);
  FastLED.addLeds<WS2812B, 14, GRB>(leds, NUM_LEDS);
  FastLED.addLeds<WS2812B, 27, GRB>(leds, NUM_LEDS);
  FastLED.addLeds<WS2812B, 26, GRB>(leds, NUM_LEDS);
  FastLED.addLeds<WS2812B, 25, GRB>(leds, NUM_LEDS);
  FastLED.addLeds<WS2812B, 33, GRB>(leds, NUM_LEDS);
  FastLED.addLeds<WS2812B, 32, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 500);
  FastLED.setBrightness(255);
  Serial.println("\nLED diag: data on GPIO 13 12 14 27 26 25 33 32");
}

void loop() {
  static const CRGB colors[] = {CRGB::Red, CRGB::Green, CRGB::Blue, CRGB::White};
  static const char *names[] = {"red", "green", "blue", "white"};
  static uint8_t i = 0;
  fill_solid(leds, NUM_LEDS, colors[i]);
  FastLED.show();
  digitalWrite(2, i & 1);
  Serial.println(names[i]);
  i = (i + 1) % 4;
  delay(1000);
}
