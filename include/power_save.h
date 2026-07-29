#pragma once

#include <cstdint>

struct PowerSaveState {
    uint32_t lastInteractionMs;
    bool active;
    bool idleAquariumStarted;
};

constexpr uint32_t kPowerSaveAquariumLeadInMs = 30000;
constexpr uint32_t kPowerSaveIdleTimeoutMs = 60000;

inline bool shouldStartIdleAquarium(uint32_t now, uint32_t lastInteractionMs,
                                    bool alreadyStarted, bool powerSaveActive,
                                    bool blocked, uint32_t leadInMs) {
    return !alreadyStarted && !powerSaveActive && !blocked &&
           now - lastInteractionMs >= leadInMs;
}

inline bool shouldEnterPowerSave(uint32_t now, uint32_t lastInteractionMs,
                                 bool alreadyActive, bool blocked,
                                 uint32_t timeoutMs) {
    return !alreadyActive && !blocked && now - lastInteractionMs >= timeoutMs;
}

inline uint8_t powerSaveBacklightLevel(bool active) {
    return active ? 0 : 255;
}

inline void notePowerSaveActivity(PowerSaveState& state, uint32_t now) {
    state.lastInteractionMs = now;
    state.active = false;
    state.idleAquariumStarted = false;
}
