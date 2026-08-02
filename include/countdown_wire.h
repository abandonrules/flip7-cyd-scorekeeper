#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Wire-safe (POD, memcpy-able) synchronized state for the Countdown game
// mode. This mirrors MastermindState's role: the countdown_engine.hpp
// classes (NumbersRound/LettersRound/ConundrumRound/CommandResult) use
// std::string/std::vector and are NOT sent over ESP-NOW directly. This
// struct captures just enough of the match state (host/guest identity,
// phase, round number, scores) to keep both boards' Countdown screens in
// sync. Round-specific puzzle content (numbers tiles, letters, conundrum
// scramble) is intentionally NOT synchronized in this pass.

enum class CountdownWirePhase : uint8_t {
    Setup = 1,
    InRound,
    BetweenRounds,
    MatchComplete,
    Exited,
};

struct CountdownWireState {
    uint32_t gameId;
    uint32_t revision;
    uint32_t hostBoardId;
    uint32_t guestBoardId;
    int32_t hostScore;
    int32_t guestScore;
    uint32_t roundNumber;
    CountdownWirePhase phase;
    uint8_t roundType;
    uint8_t reserved[2];
};

inline bool sameCountdownWireState(const CountdownWireState& first,
                                   const CountdownWireState& second) {
    return memcmp(&first, &second, sizeof(CountdownWireState)) == 0;
}

inline bool isValidCountdownWireState(const CountdownWireState& state) {
    if (state.gameId == 0 || state.hostBoardId == 0 ||
        state.guestBoardId == 0 ||
        state.hostBoardId == state.guestBoardId ||
        state.phase < CountdownWirePhase::Setup ||
        state.phase > CountdownWirePhase::Exited) {
        return false;
    }
    for (uint8_t value : state.reserved) {
        if (value != 0) {
            return false;
        }
    }
    return true;
}

inline CountdownWireState makeCountdownMatch(uint32_t hostBoardId,
                                             uint32_t guestBoardId,
                                             uint32_t gameId) {
    CountdownWireState state{};
    state.gameId = gameId;
    state.hostBoardId = hostBoardId;
    state.guestBoardId = guestBoardId;
    state.roundNumber = 1;
    state.phase = CountdownWirePhase::Setup;
    return state;
}

enum class CountdownVersionOrder : uint8_t { Older, Same, Newer };

inline CountdownVersionOrder compareCountdownVersion(
    const CountdownWireState& current, const CountdownWireState& candidate) {
    if (candidate.gameId != current.gameId) {
        return candidate.gameId > current.gameId
                   ? CountdownVersionOrder::Newer
                   : CountdownVersionOrder::Older;
    }
    if (candidate.revision != current.revision) {
        return candidate.revision > current.revision
                   ? CountdownVersionOrder::Newer
                   : CountdownVersionOrder::Older;
    }
    return CountdownVersionOrder::Same;
}

inline uint32_t countdownStateDigest(const CountdownWireState& state) {
    uint32_t digest = 2166136261u;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
    for (size_t index = 0; index < sizeof(state); ++index) {
        digest ^= bytes[index];
        digest *= 16777619u;
    }
    return digest;
}

inline bool shouldAdoptCountdownState(const CountdownWireState& current,
                                      const CountdownWireState& candidate) {
    return isValidCountdownWireState(candidate) &&
           compareCountdownVersion(current, candidate) ==
               CountdownVersionOrder::Newer;
}

inline bool isCountdownDeliverySuperseded(const CountdownWireState& pending,
                                          const CountdownWireState& observed) {
    return pending.gameId == observed.gameId &&
           pending.revision < observed.revision;
}

inline bool exitCountdownMatch(CountdownWireState& state,
                               uint32_t actorBoardId) {
    if (state.phase == CountdownWirePhase::Exited ||
        (actorBoardId != state.hostBoardId &&
         actorBoardId != state.guestBoardId)) {
        return false;
    }
    ++state.revision;
    state.phase = CountdownWirePhase::Exited;
    return true;
}

inline bool applyCountdownExitSignal(CountdownWireState& current,
                                     const CountdownWireState& candidate,
                                     uint32_t senderBoardId) {
    if (!isValidCountdownWireState(current) ||
        !isValidCountdownWireState(candidate) ||
        candidate.phase != CountdownWirePhase::Exited ||
        current.gameId != candidate.gameId ||
        current.hostBoardId != candidate.hostBoardId ||
        current.guestBoardId != candidate.guestBoardId) {
        return false;
    }
    if (current.phase == CountdownWirePhase::Exited) {
        return senderBoardId == current.hostBoardId ||
               senderBoardId == current.guestBoardId;
    }
    return exitCountdownMatch(current, senderBoardId);
}

static_assert(sizeof(CountdownWireState) == 32,
              "CountdownWireState wire format changed");
static_assert(offsetof(CountdownWireState, gameId) == 0,
              "CountdownWireState gameId offset changed");
static_assert(offsetof(CountdownWireState, revision) == 4,
              "CountdownWireState revision offset changed");
static_assert(offsetof(CountdownWireState, hostBoardId) == 8,
              "CountdownWireState hostBoardId offset changed");
static_assert(offsetof(CountdownWireState, guestBoardId) == 12,
              "CountdownWireState guestBoardId offset changed");
static_assert(offsetof(CountdownWireState, hostScore) == 16,
              "CountdownWireState hostScore offset changed");
static_assert(offsetof(CountdownWireState, guestScore) == 20,
              "CountdownWireState guestScore offset changed");
static_assert(offsetof(CountdownWireState, roundNumber) == 24,
              "CountdownWireState roundNumber offset changed");
static_assert(offsetof(CountdownWireState, phase) == 28,
              "CountdownWireState phase offset changed");
static_assert(offsetof(CountdownWireState, roundType) == 29,
              "CountdownWireState roundType offset changed");
