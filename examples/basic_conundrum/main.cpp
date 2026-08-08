#include <Arduino.h>
#include <TFT_eSPI.h>

#include "DebugCanvas.h"

TFT_eSPI tft;
DebugCanvas canvas(tft);

namespace {
uint32_t frameNumber = 0;

void drawConundrumScreen(
    const char* scramble,
    const char* hint)
{
    canvas.setScreenName("ConundrumRound");
    canvas.clear(TFT_NAVY);

    TFT_eSprite& ui = canvas.graphics();

    ui.setTextDatum(TC_DATUM);
    ui.setTextColor(TFT_WHITE, TFT_NAVY);
    ui.drawString(
        "CONUNDRUM",
        DebugCanvas::WIDTH / 2,
        10,
        2);

    constexpr int tileWidth = 24;
    constexpr int tileHeight = 34;
    constexpr int tileGap = 2;
    constexpr int totalWidth =
        (tileWidth * 9) + (tileGap * 8);
    constexpr int startX =
        (DebugCanvas::WIDTH - totalWidth) / 2;
    constexpr int tileY = 60;

    for (int index = 0; index < 9; ++index) {
        const int x =
            startX + index * (tileWidth + tileGap);

        ui.fillRoundRect(
            x,
            tileY,
            tileWidth,
            tileHeight,
            3,
            TFT_WHITE);

        ui.drawRoundRect(
            x,
            tileY,
            tileWidth,
            tileHeight,
            3,
            TFT_DARKGREY);

        char letter[2] = {
            scramble[index],
            '\0'
        };

        ui.setTextDatum(MC_DATUM);
        ui.setTextColor(TFT_BLACK, TFT_WHITE);
        ui.drawString(
            letter,
            x + tileWidth / 2,
            tileY + tileHeight / 2,
            2);
    }

    ui.setTextDatum(TC_DATUM);
    ui.setTextColor(TFT_YELLOW, TFT_NAVY);
    ui.drawString(
        "HINT",
        DebugCanvas::WIDTH / 2,
        125,
        2);

    ui.setTextColor(TFT_WHITE, TFT_NAVY);
    ui.drawCentreString(
        hint,
        DebugCanvas::WIDTH / 2,
        155,
        2);

    canvas.drawDebugOverlay(++frameNumber);
    canvas.present();
}
}

void setup()
{
    Serial.begin(115200);

    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);

    if (!canvas.begin()) {
        Serial.println(
            "[Fatal] Could not allocate debug framebuffer.");

        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString(
            "Framebuffer error",
            10,
            20,
            2);
        return;
    }

    canvas.printDebugInfo();

    drawConundrumScreen(
        "FASTBREAK",
        "The first meal of the day");
}

void loop()
{
}
