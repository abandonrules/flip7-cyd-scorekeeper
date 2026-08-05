#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Wire-safe (POD, memcpy-able) synchronized state for the Countdown game mode.
//
// Identity fields
//   hostBoardId   — the score leader; owns authoritative game state.
//   guestBoardId  — the non-leader player.
//   chooserBoardId — the previous round winner; picks the next round type.
//                    Initialised to hostBoardId; retained on no-winner rounds.
//   hostTerm      — incremented on every host-authority transfer; authoritative
//                    packets with a stale hostTerm are rejected.
//
// Round-specific puzzle content (tiles, letters, conundrum scramble) is
// transmitted via CountdownFullStatePacket, not stored here.

enum class CountdownWirePhase : uint8_t {
    Setup         = 1,
    InRound,
    BetweenRounds,
    MatchComplete,
    Exited,
};

// Sub-phase within InRound.  Stored in roundSubPhase; 0 = none.
enum class CountdownRoundSubPhase : uint8_t {
    None                 = 0,
    Intro                = 1,  // 30-second get-ready animation (all round types)
    NumPicking           = 2,  // chooser selects large-number count
    NumThinking          = 3,  // 30-second silent calculation
    NumClaimEntry        = 4,  // private achieved-value entry
    NumClaimReveal       = 5,  // both values revealed simultaneously
    NumPresentPlayerA    = 6,  // first presenter (closer to target)
    NumPresentPlayerB    = 7,  // second presenter
    NumResult            = 8,
    LetPicking           = 9,  // host draws vowels and consonants
    LetThinking          = 10, // 30-second solve timer
    LetClaimEntry        = 11, // private word-length claim
    LetClaimReveal       = 12,
    LetPresentPlayerA    = 13,
    LetPresentPlayerB    = 14,
    LetResult            = 15,
    ConReady             = 16, // conundrum intro
    ConActive            = 17, // circular tiles visible; both players attempt
    ConResult            = 18, // someone solved it
    ConResultNoWinner    = 19, // timeout; solution revealed; no points
    HostTransfer         = 20, // old host sending committed snapshot
    HostTransferAck      = 21, // new host acknowledged
};

struct CountdownWireState {
    uint32_t gameId;          // offset  0
    uint32_t revision;        // offset  4
    uint32_t hostBoardId;     // offset  8
    uint32_t guestBoardId;    // offset 12
    uint32_t chooserBoardId;  // offset 16 (previous round winner)
    uint32_t hostTerm;        // offset 20 (incremented on authority transfer)
    int32_t  hostScore;       // offset 24
    int32_t  guestScore;      // offset 28
    uint32_t roundNumber;     // offset 32
    CountdownWirePhase phase; // offset 36
    uint8_t  roundType;       // offset 37
    uint8_t  roundSubPhase;   // offset 38  (CountdownRoundSubPhase cast)
    uint8_t  reserved;        // offset 39  must be zero
};

// Host-authority record — carried in transfer packets; every authoritative
// packet includes hostTerm so delayed old-host packets can be rejected.
struct HostAuthority {
    uint32_t hostBoardId;
    uint32_t hostTerm;
    uint32_t nextSequenceNumber;
};

inline bool sameCountdownWireState(const CountdownWireState& first,
                                   const CountdownWireState& second) {
    return memcmp(&first, &second, sizeof(CountdownWireState)) == 0;
}

inline bool isValidCountdownWireState(const CountdownWireState& state) {
    if (state.gameId == 0 || state.hostBoardId == 0 ||
        state.guestBoardId == 0 ||
        state.hostBoardId == state.guestBoardId ||
        state.hostTerm == 0 ||
        (state.chooserBoardId != 0 &&
         state.chooserBoardId != state.hostBoardId &&
         state.chooserBoardId != state.guestBoardId) ||
        state.phase < CountdownWirePhase::Setup ||
        state.phase > CountdownWirePhase::Exited ||
        state.reserved != 0) {
        return false;
    }
    return true;
}

inline CountdownWireState makeCountdownMatch(uint32_t hostBoardId,
                                             uint32_t guestBoardId,
                                             uint32_t gameId) {
    CountdownWireState state{};
    state.gameId         = gameId;
    state.revision       = 1;
    state.hostBoardId    = hostBoardId;
    state.guestBoardId   = guestBoardId;
    state.chooserBoardId = hostBoardId; // host chooses the first round
    state.hostTerm       = 1;
    state.roundNumber    = 1;
    state.phase          = CountdownWirePhase::Setup;
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

inline bool advanceCountdownRound(CountdownWireState& state,
                                  uint32_t actorBoardId) {
    if (state.phase == CountdownWirePhase::Exited ||
        actorBoardId != state.hostBoardId || state.roundNumber == UINT32_MAX) {
        return false;
    }
    ++state.roundNumber;
    ++state.revision;
    state.phase = CountdownWirePhase::BetweenRounds;
    return true;
}

inline bool selectCountdownRoundType(CountdownWireState& state,
                                     uint32_t actorBoardId,
                                     uint8_t roundType) {
    if (state.phase != CountdownWirePhase::BetweenRounds ||
        actorBoardId != state.chooserBoardId ||  // chooser picks, not host
        roundType < 1 || roundType > 3) {
        return false;
    }
    state.roundType      = roundType;
    state.roundSubPhase  = static_cast<uint8_t>(CountdownRoundSubPhase::Intro);
    state.phase          = CountdownWirePhase::InRound;
    ++state.revision;
    return true;
}

// Advance to a new round sub-phase.  Host-only.
inline bool advanceCountdownSubPhase(CountdownWireState& state,
                                     uint32_t actorBoardId,
                                     CountdownRoundSubPhase next) {
    if (actorBoardId != state.hostBoardId ||
        state.phase != CountdownWirePhase::InRound) {
        return false;
    }
    state.roundSubPhase = static_cast<uint8_t>(next);
    ++state.revision;
    return true;
}

// Apply a committed host-transfer snapshot.  Both boards increment hostTerm.
inline bool applyCountdownHostTransfer(CountdownWireState& state,
                                       uint32_t newHostBoardId,
                                       uint32_t expectedHostTerm) {
    if (state.hostTerm != expectedHostTerm ||
        (newHostBoardId != state.hostBoardId &&
         newHostBoardId != state.guestBoardId)) {
        return false;
    }
    if (newHostBoardId != state.hostBoardId) {
        // Swap host and guest identity
        uint32_t tmp    = state.hostBoardId;
        state.hostBoardId  = state.guestBoardId;
        state.guestBoardId = tmp;
        int32_t  tmpScore  = state.hostScore;
        state.hostScore    = state.guestScore;
        state.guestScore   = tmpScore;
    }
    ++state.hostTerm;
    ++state.revision;
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

static_assert(sizeof(CountdownWireState) == 40,
              "CountdownWireState wire format changed");
static_assert(offsetof(CountdownWireState, gameId) == 0,
              "CountdownWireState gameId offset changed");
static_assert(offsetof(CountdownWireState, revision) == 4,
              "CountdownWireState revision offset changed");
static_assert(offsetof(CountdownWireState, hostBoardId) == 8,
              "CountdownWireState hostBoardId offset changed");
static_assert(offsetof(CountdownWireState, guestBoardId) == 12,
              "CountdownWireState guestBoardId offset changed");
static_assert(offsetof(CountdownWireState, chooserBoardId) == 16,
              "CountdownWireState chooserBoardId offset changed");
static_assert(offsetof(CountdownWireState, hostTerm) == 20,
              "CountdownWireState hostTerm offset changed");
static_assert(offsetof(CountdownWireState, hostScore) == 24,
              "CountdownWireState hostScore offset changed");
static_assert(offsetof(CountdownWireState, guestScore) == 28,
              "CountdownWireState guestScore offset changed");
static_assert(offsetof(CountdownWireState, roundNumber) == 32,
              "CountdownWireState roundNumber offset changed");
static_assert(offsetof(CountdownWireState, phase) == 36,
              "CountdownWireState phase offset changed");
static_assert(offsetof(CountdownWireState, roundType) == 37,
              "CountdownWireState roundType offset changed");
static_assert(offsetof(CountdownWireState, roundSubPhase) == 38,
              "CountdownWireState roundSubPhase offset changed");
