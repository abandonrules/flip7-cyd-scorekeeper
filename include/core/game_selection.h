#pragma once

#include <algorithm>
#include <cstdint>

constexpr uint32_t kNoActiveGameEpoch = 0;

// Identifies which game is currently active (or Home if idle).
enum class ActiveGameKind : uint8_t {
    Home = 0,
    Puzzle,
    Mastermind,
};

// Tracks the currently active game: a monotonically-increasing epoch and kind.
struct ActiveGameClock {
    uint32_t epoch;
    ActiveGameKind kind;
};

// Returns true when an incoming game state packet should be applied locally.
inline bool shouldAcceptGameState(const ActiveGameClock& active,
                                  ActiveGameKind incomingKind,
                                  uint32_t incomingEpoch,
                                  bool terminalState = false) {
    if (incomingEpoch != active.epoch) {
        return incomingEpoch > active.epoch;
    }
    return incomingKind == active.kind ||
           (active.kind == ActiveGameKind::Home && terminalState);
}

// Returns true when a completed game may safely transition back to the Home screen.
inline bool canReturnCompletedGameHome(const ActiveGameClock& active,
                                       ActiveGameKind completedGame,
                                       bool deliveryPending) {
    return active.kind == completedGame && !deliveryPending;
}

// Returns true when a state packet should be sent; guards against transmitting
// stale or mismatched game state over ESP-NOW.
inline bool shouldSendActiveGameState(
    const ActiveGameClock& active, ActiveGameKind stateKind,
    uint32_t stateEpoch, bool snapshotMatchesCurrent,
    bool terminalDelivery = false) {
    const bool activeKind = active.kind == stateKind;
    const bool terminalFromHome =
        active.kind == ActiveGameKind::Home && terminalDelivery;
    return (activeKind || terminalFromHome) && active.epoch == stateEpoch &&
           snapshotMatchesCurrent;
}

// Produces the next game epoch (max of all known epochs + 1).
// Returns kNoActiveGameEpoch on overflow.
inline uint32_t nextActiveGameEpoch(uint32_t activeEpoch,
                                    uint32_t puzzleEpoch,
                                    uint32_t mastermindEpoch) {
    const uint32_t latest =
        std::max(activeEpoch, std::max(puzzleEpoch, mastermindEpoch));
    return latest == UINT32_MAX ? kNoActiveGameEpoch : latest + 1;
}
