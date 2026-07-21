#include <Arduino.h>
#include <TFT_eSPI.h>

namespace {
constexpr uint8_t kBacklightPin = 21;
TFT_eSPI display;
}

void setup() {
    Serial.begin(115200);

    pinMode(kBacklightPin, OUTPUT);
    digitalWrite(kBacklightPin, HIGH);

    display.init();
    display.setRotation(1);
    display.fillScreen(TFT_NAVY);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    display.setTextDatum(MC_DATUM);
    display.drawString("Hello, World!", display.width() / 2,
                       display.height() / 2, 4);

    Serial.println("Hello, World! displayed on CYD");
}

void loop() {
    delay(1000);
}
