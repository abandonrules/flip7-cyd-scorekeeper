#include "DebugCanvas.h"

#include <esp_heap_caps.h>

DebugCanvas::DebugCanvas(TFT_eSPI& display)
    : display_(display),
      sprite_(&display)
{
}

bool DebugCanvas::begin(uint8_t preferredDepth)
{
    if (preferredDepth != 8 && preferredDepth != 16) {
        preferredDepth = 16;
    }

    ready_ = false;
    colorDepth_ = 0;
    width_ = display_.width();
    height_ = display_.height();

    if (allocate(preferredDepth)) {
        Serial.printf(
            "[DebugCanvas] Allocated %dx%d %u-bit framebuffer.\n",
            width_, height_, preferredDepth);
        return true;
    }

    if (preferredDepth == 8) {
        Serial.printf(
            "[DebugCanvas] ERROR: %dx%d 8-bit framebuffer allocation failed.\n",
            width_, height_);
        return false;
    }

    Serial.printf(
        "[DebugCanvas] %u-bit allocation failed; trying 8-bit.\n",
        preferredDepth);

    if (allocate(8)) {
        Serial.printf(
            "[DebugCanvas] Allocated %dx%d 8-bit framebuffer.\n",
            width_, height_);
        return true;
    }

    Serial.println(
        "[DebugCanvas] ERROR: framebuffer allocation failed.");

    return false;
}

bool DebugCanvas::allocate(uint8_t depth)
{
    sprite_.deleteSprite();
    sprite_.setColorDepth(depth);

    void* buffer = sprite_.createSprite(width_, height_);

    if (buffer == nullptr) {
        return false;
    }

    ready_ = true;
    colorDepth_ = depth;

    sprite_.setSwapBytes(true);
    sprite_.fillSprite(TFT_BLACK);

    return true;
}

void DebugCanvas::clear(uint32_t color)
{
    if (!ready_) {
        return;
    }

    sprite_.fillSprite(color);
}

void DebugCanvas::present()
{
    if (!ready_) {
        return;
    }

    const uint32_t startedAt = micros();

    sprite_.pushSprite(0, 0);

    const uint32_t elapsed = micros() - startedAt;

    Serial.printf(
        "[DebugCanvas] Presented '%s' in %lu us\n",
        screenName_,
        static_cast<unsigned long>(elapsed));
}

TFT_eSprite& DebugCanvas::graphics()
{
    return sprite_;
}

const TFT_eSprite& DebugCanvas::graphics() const
{
    return sprite_;
}

bool DebugCanvas::isReady() const
{
    return ready_;
}

int16_t DebugCanvas::width() const
{
    return width_;
}

int16_t DebugCanvas::height() const
{
    return height_;
}

uint8_t DebugCanvas::colorDepth() const
{
    return colorDepth_;
}

size_t DebugCanvas::bufferSizeBytes() const
{
    if (!ready_) {
        return 0;
    }

    const size_t pixelCount =
        static_cast<size_t>(width_) *
        static_cast<size_t>(height_);

    if (colorDepth_ == 16) {
        return pixelCount * sizeof(uint16_t);
    }

    return pixelCount;
}

void DebugCanvas::setScreenName(const char* screenName)
{
    screenName_ =
        screenName != nullptr ? screenName : "Unknown";
}

const char* DebugCanvas::screenName() const
{
    return screenName_;
}

void DebugCanvas::drawDebugOverlay(uint32_t frameNumber)
{
    if (!ready_) {
        return;
    }

    constexpr int overlayHeight = 18;

    sprite_.fillRect(
        0,
        height_ - overlayHeight,
        width_,
        overlayHeight,
        TFT_BLACK);

    sprite_.drawFastHLine(
        0,
        height_ - overlayHeight,
        width_,
        TFT_DARKGREY);

    sprite_.setTextDatum(ML_DATUM);
    sprite_.setTextColor(TFT_GREEN, TFT_BLACK);

    char status[80];

    snprintf(
        status,
        sizeof(status),
        "%s | F:%lu | H:%uK",
        screenName_,
        static_cast<unsigned long>(frameNumber),
        static_cast<unsigned>(ESP.getFreeHeap() / 1024));

    sprite_.drawString(
        status,
        3,
        height_ - overlayHeight / 2,
        1);
}

void DebugCanvas::printDebugInfo(Stream& output) const
{
    output.println("-------- DebugCanvas --------");
    output.printf("Ready:       %s\n", ready_ ? "yes" : "no");
    output.printf("Screen:      %s\n", screenName_);
    output.printf(
        "Dimensions:  %d x %d\n",
        width_,
        height_);
    output.printf(
        "Color depth: %u-bit\n",
        colorDepth_);
    output.printf(
        "Buffer:      %u bytes\n",
        static_cast<unsigned>(bufferSizeBytes()));
    output.printf("Free heap:   %u bytes\n", ESP.getFreeHeap());
    output.printf(
        "Largest RAM: %u bytes\n",
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    output.println("-----------------------------");
}

size_t DebugCanvas::rleFrameSize(const void* pixels, size_t pixelCount) const
{
    if (pixels == nullptr || pixelCount == 0) {
        return 0;
    }

    size_t runCount = 1;
    if (colorDepth_ == 8) {
        const auto* values = static_cast<const uint8_t*>(pixels);
        uint8_t runLength = 1;
        for (size_t index = 1; index < pixelCount; ++index) {
            if (values[index] == values[index - 1] && runLength < UINT8_MAX) {
                ++runLength;
            } else {
                ++runCount;
                runLength = 1;
            }
        }
        return runCount * 2;
    }

    const auto* values = static_cast<const uint16_t*>(pixels);
    uint8_t runLength = 1;
    for (size_t index = 1; index < pixelCount; ++index) {
        if (values[index] == values[index - 1] && runLength < UINT8_MAX) {
            ++runLength;
        } else {
            ++runCount;
            runLength = 1;
        }
    }
    return runCount * 3;
}

bool DebugCanvas::writeRleFrame(Stream& output)
{
    if (!ready_) {
        output.println("DCFRAME_ERROR canvas-not-ready");
        return false;
    }

    const size_t pixelCount =
        static_cast<size_t>(width_) * static_cast<size_t>(height_);
    const void* pixels = sprite_.getPointer();
    const size_t encodedSize = rleFrameSize(pixels, pixelCount);
    if (pixels == nullptr || encodedSize == 0) {
        output.println("DCFRAME_ERROR framebuffer-unavailable");
        return false;
    }

    output.printf("DCFRAME 1 %d %d %u %u %s\n",
                  width_, height_, colorDepth_,
                  static_cast<unsigned>(encodedSize), screenName_);

    if (colorDepth_ == 8) {
        const auto* values = static_cast<const uint8_t*>(pixels);
        for (size_t index = 0; index < pixelCount;) {
            uint8_t runLength = 1;
            while (index + runLength < pixelCount &&
                   values[index + runLength] == values[index] &&
                   runLength < UINT8_MAX) {
                ++runLength;
            }
            output.write(runLength);
            output.write(values[index]);
            index += runLength;
        }
    } else {
        const auto* values = static_cast<const uint16_t*>(pixels);
        for (size_t index = 0; index < pixelCount;) {
            uint8_t runLength = 1;
            while (index + runLength < pixelCount &&
                   values[index + runLength] == values[index] &&
                   runLength < UINT8_MAX) {
                ++runLength;
            }
            output.write(runLength);
            output.write(reinterpret_cast<const uint8_t*>(&values[index]),
                         sizeof(values[index]));
            index += runLength;
        }
    }
    output.flush();
    return true;
}
