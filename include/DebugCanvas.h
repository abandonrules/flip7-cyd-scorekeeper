#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

class DebugCanvas {
public:
    static constexpr int16_t WIDTH = 240;
    static constexpr int16_t HEIGHT = 320;

    explicit DebugCanvas(TFT_eSPI& display);

    bool begin(uint8_t preferredDepth = 16);
    void clear(uint32_t color = TFT_BLACK);
    void present();

    TFT_eSprite& graphics();
    const TFT_eSprite& graphics() const;

    bool isReady() const;
    int16_t width() const;
    int16_t height() const;
    uint8_t colorDepth() const;
    size_t bufferSizeBytes() const;

    void setScreenName(const char* screenName);
    const char* screenName() const;

    void drawDebugOverlay(uint32_t frameNumber);
    void printDebugInfo(Stream& output = Serial) const;
    bool writeRleFrame(Stream& output = Serial);

private:
    TFT_eSPI& display_;
    TFT_eSprite sprite_;

    bool ready_ = false;
    int16_t width_ = WIDTH;
    int16_t height_ = HEIGHT;
    uint8_t colorDepth_ = 0;
    const char* screenName_ = "Unknown";

    bool allocate(uint8_t depth);
    size_t rleFrameSize(const void* pixels, size_t pixelCount) const;
};
