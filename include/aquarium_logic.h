#pragma once

#include <cstdint>

constexpr int16_t kAquariumMinFishY = 45;
constexpr int16_t kAquariumMaxFishY = 198;
constexpr uint8_t kAquariumFullHunger = 0;
constexpr uint8_t kAquariumMaxHunger = 255;
constexpr uint32_t kAquariumHungerTickMs = 1400;

inline int16_t aquariumInitialFishX(uint8_t index) {
    return static_cast<int16_t>(24 + (index * 43) % 270);
}

inline int16_t aquariumInitialFishY(uint8_t index) {
    return static_cast<int16_t>(54 + ((index * 53) % 136));
}

inline int8_t aquariumInitialFishVx(uint8_t index) {
    return static_cast<int8_t>((index % 2 == 0) ? 2 : -2);
}

inline int8_t aquariumInitialFishVy(uint8_t index) {
    return static_cast<int8_t>((index % 3) - 1);
}

inline uint8_t aquariumInitialHunger(uint8_t index) {
    return static_cast<uint8_t>(80 + (index * 23) % 80);
}

inline uint8_t aquariumDrainSatiety(uint8_t hunger, uint32_t elapsedMs) {
    const uint32_t ticks = elapsedMs / kAquariumHungerTickMs;
    const uint32_t next = static_cast<uint32_t>(hunger) + ticks;
    return next > kAquariumMaxHunger ? kAquariumMaxHunger : static_cast<uint8_t>(next);
}

inline uint8_t aquariumSatisfyHunger(uint8_t hunger, uint8_t amount = 96) {
    return hunger > amount ? static_cast<uint8_t>(hunger - amount) : kAquariumFullHunger;
}

inline bool aquariumFishShouldChaseFood(uint8_t hunger, int16_t fishX, int16_t fishY,
                                        int16_t foodX, int16_t foodY) {
    const int16_t chaseX = hunger > 180 ? 105 : 70;
    const int16_t chaseY = hunger > 180 ? 72 : 55;
    return hunger > 32 && foodX - fishX < chaseX && fishX - foodX < chaseX &&
           foodY - fishY < chaseY && fishY - foodY < chaseY;
}

inline bool aquariumFishReachedFood(int16_t fishX, int16_t fishY, int16_t foodX,
                                    int16_t foodY) {
    return foodX - fishX < 12 && fishX - foodX < 12 &&
           foodY - fishY < 10 && fishY - foodY < 10;
}

inline bool aquariumSnapshotLooksCollapsed(const int16_t* x, const int16_t* y,
                                           uint8_t count) {
    if (count < 2) {
        return false;
    }
    for (uint8_t i = 0; i < count; ++i) {
        if (x[i] < -40 || x[i] > 340 || y[i] < kAquariumMinFishY ||
            y[i] > kAquariumMaxFishY) {
            return true;
        }
    }
    uint8_t closePairs = 0;
    for (uint8_t i = 0; i < count; ++i) {
        for (uint8_t j = i + 1; j < count; ++j) {
            const int16_t dx = x[i] > x[j] ? x[i] - x[j] : x[j] - x[i];
            const int16_t dy = y[i] > y[j] ? y[i] - y[j] : y[j] - y[i];
            if (dx < 14 && dy < 10) {
                ++closePairs;
            }
        }
    }
    return closePairs >= count - 1;
}
