#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

constexpr uint8_t kMastermindCodeLength = 4;
constexpr uint8_t kMastermindColorCount = 6;
constexpr uint8_t kMastermindMaxGuesses = 10;

// Phase of a Mastermind match — SecretEntry → Guessing → RoundComplete → Exited.
enum class MastermindPhase : uint8_t {
    SecretEntry = 1,
    Guessing,
    RoundComplete,
    Exited,
};

// A four-peg colour sequence used for both the secret code and each guess.
struct MastermindCode {
    uint8_t colors[kMastermindCodeLength];
};

// Feedback for a single guess: exact = right colour + right position,
// colorOnly = right colour but wrong position.
struct MastermindFeedback {
    uint8_t exact;
    uint8_t colorOnly;
};

// A single guess entry combining the submitted code with its computed feedback.
struct MastermindGuess {
    MastermindCode code;
    MastermindFeedback feedback;
};

// Full wire-safe match state for one Mastermind game between two boards.
struct MastermindState {
    MastermindGuess guesses[kMastermindMaxGuesses];
    MastermindCode secret;
    uint32_t gameId;
    uint32_t revision;
    uint32_t hostBoardId;
    uint32_t guestBoardId;
    uint32_t codemakerBoardId;
    uint32_t roundWinnerBoardId;
    uint8_t hostScore;
    uint8_t guestScore;
    uint8_t round;
    uint8_t guessCount;
    MastermindPhase phase;
    uint8_t reserved[3];
};

// Returns true when every peg in the code is zero (unset).
inline bool isEmptyMastermindCode(const MastermindCode& code) {
    for (uint8_t color : code.colors) {
        if (color != 0) {
            return false;
        }
    }
    return true;
}

// Returns true when every peg holds a colour in [1, kMastermindColorCount].
inline bool isValidMastermindCode(const MastermindCode& code) {
    for (uint8_t color : code.colors) {
        if (color == 0 || color > kMastermindColorCount) {
            return false;
        }
    }
    return true;
}

// Scores a guess against the secret using per-colour frequency buckets.
inline MastermindFeedback evaluateMastermindGuess(
    const MastermindCode& secret, const MastermindCode& guess) {
    MastermindFeedback feedback{};
    uint8_t secretCounts[kMastermindColorCount + 1]{};
    uint8_t guessCounts[kMastermindColorCount + 1]{};
    for (uint8_t index = 0; index < kMastermindCodeLength; ++index) {
        if (secret.colors[index] == guess.colors[index]) {
            ++feedback.exact;
        } else {
            ++secretCounts[secret.colors[index]];
            ++guessCounts[guess.colors[index]];
        }
    }
    for (uint8_t color = 1; color <= kMastermindColorCount; ++color) {
        feedback.colorOnly += secretCounts[color] < guessCounts[color]
                                  ? secretCounts[color]
                                  : guessCounts[color];
    }
    return feedback;
}

// Returns the board ID of the codebreaker (the player who is NOT the codemaker).
inline uint32_t mastermindCodebreakerBoardId(const MastermindState& state) {
    return state.codemakerBoardId == state.hostBoardId ? state.guestBoardId
                                                       : state.hostBoardId;
}

// Creates a fresh MastermindState with the host as initial codemaker.
inline MastermindState makeMastermindMatch(uint32_t hostBoardId,
                                           uint32_t guestBoardId,
                                           uint32_t gameId) {
    MastermindState state{};
    state.gameId = gameId;
    state.hostBoardId = hostBoardId;
    state.guestBoardId = guestBoardId;
    state.codemakerBoardId = hostBoardId;
    state.round = 1;
    state.phase = MastermindPhase::SecretEntry;
    return state;
}

// Returns true when both codes have identical peg colours.
inline bool sameMastermindCode(const MastermindCode& first,
                               const MastermindCode& second) {
    return memcmp(first.colors, second.colors, kMastermindCodeLength) == 0;
}

// Returns true when both states are byte-for-byte identical.
inline bool sameMastermindState(const MastermindState& first,
                                const MastermindState& second) {
    return memcmp(&first, &second, sizeof(MastermindState)) == 0;
}

// Deep-validates the state: IDs, phase, role parity, score accounting,
// reserved bytes, guess/feedback consistency, and round-winner correctness.
inline bool isValidMastermindState(const MastermindState& state) {
    if (state.gameId == 0 || state.hostBoardId == 0 ||
        state.guestBoardId == 0 ||
        state.hostBoardId == state.guestBoardId ||
        (state.codemakerBoardId != state.hostBoardId &&
         state.codemakerBoardId != state.guestBoardId) || state.round == 0 ||
        state.codemakerBoardId !=
            (state.round % 2 == 1 ? state.hostBoardId
                                  : state.guestBoardId) ||
        state.guessCount > kMastermindMaxGuesses ||
        state.phase < MastermindPhase::SecretEntry ||
        state.phase > MastermindPhase::Exited) {
        return false;
    }
    const uint16_t scoreTotal =
        static_cast<uint16_t>(state.hostScore) + state.guestScore;
    const uint16_t completedRounds =
        static_cast<uint16_t>(state.round - 1) +
        (state.phase == MastermindPhase::RoundComplete ? 1 : 0);
    const bool validExitedScore = state.phase == MastermindPhase::Exited &&
        (scoreTotal == state.round - 1 || scoreTotal == state.round);
    if (scoreTotal != completedRounds && !validExitedScore) {
        return false;
    }
    for (uint8_t value : state.reserved) {
        if (value != 0) {
            return false;
        }
    }
    const MastermindGuess emptyGuess{};
    for (uint8_t index = state.guessCount;
         index < kMastermindMaxGuesses; ++index) {
        if (memcmp(&state.guesses[index], &emptyGuess,
                   sizeof(emptyGuess)) != 0) {
            return false;
        }
    }
    if (state.phase == MastermindPhase::SecretEntry ||
        state.phase == MastermindPhase::Exited) {
        return state.guessCount == 0 && state.roundWinnerBoardId == 0 &&
               isEmptyMastermindCode(state.secret);
    }
    if (!isValidMastermindCode(state.secret) ||
        (state.guessCount == 0 &&
         state.phase == MastermindPhase::RoundComplete)) {
        return false;
    }
    for (uint8_t index = 0; index < state.guessCount; ++index) {
        if (!isValidMastermindCode(state.guesses[index].code)) {
            return false;
        }
        const MastermindFeedback expected =
            evaluateMastermindGuess(state.secret, state.guesses[index].code);
        if (state.guesses[index].feedback.exact != expected.exact ||
            state.guesses[index].feedback.colorOnly != expected.colorOnly) {
            return false;
        }
    }
    if (state.phase == MastermindPhase::Guessing) {
        return state.guessCount < kMastermindMaxGuesses &&
               state.roundWinnerBoardId == 0 &&
               (state.guessCount == 0 ||
                state.guesses[state.guessCount - 1].feedback.exact <
                    kMastermindCodeLength);
    }
    const bool solved =
        state.guesses[state.guessCount - 1].feedback.exact ==
        kMastermindCodeLength;
    const uint32_t expectedWinner =
        solved ? mastermindCodebreakerBoardId(state)
               : state.codemakerBoardId;
    return state.roundWinnerBoardId == expectedWinner &&
           (solved || state.guessCount == kMastermindMaxGuesses);
}

// Transitions SecretEntry → Guessing; only the codemaker may set the secret.
inline bool submitMastermindSecret(MastermindState& state,
                                   const MastermindCode& secret,
                                   uint32_t actorBoardId) {
    if (state.phase != MastermindPhase::SecretEntry ||
        actorBoardId != state.codemakerBoardId ||
        !isValidMastermindCode(secret)) {
        return false;
    }
    state.secret = secret;
    state.phase = MastermindPhase::Guessing;
    ++state.revision;
    return true;
}

// Records a guess with computed feedback; transitions to RoundComplete on
// a correct answer or when the maximum guess count is reached.
inline bool submitMastermindGuess(MastermindState& state,
                                  const MastermindCode& guess,
                                  uint32_t actorBoardId) {
    if (state.phase != MastermindPhase::Guessing ||
        actorBoardId == state.codemakerBoardId ||
        !isValidMastermindCode(guess) ||
        state.guessCount >= kMastermindMaxGuesses) {
        return false;
    }
    MastermindGuess& record = state.guesses[state.guessCount++];
    record.code = guess;
    record.feedback = evaluateMastermindGuess(state.secret, guess);
    ++state.revision;
    if (record.feedback.exact == kMastermindCodeLength ||
        state.guessCount == kMastermindMaxGuesses) {
        state.phase = MastermindPhase::RoundComplete;
        if (record.feedback.exact == kMastermindCodeLength) {
            state.roundWinnerBoardId = actorBoardId;
        } else {
            state.roundWinnerBoardId = state.codemakerBoardId;
        }
        if (state.roundWinnerBoardId == state.hostBoardId) {
            ++state.hostScore;
        } else {
            ++state.guestScore;
        }
    }
    return true;
}

// Clears the round, swaps codemaker/codebreaker roles, and advances to the
// next round's SecretEntry phase. Host-only.
inline bool advanceMastermindRound(MastermindState& state,
                                   uint32_t actorBoardId) {
    if (state.phase != MastermindPhase::RoundComplete ||
        actorBoardId != state.hostBoardId || state.round == UINT8_MAX) {
        return false;
    }
    state.codemakerBoardId = mastermindCodebreakerBoardId(state);
    memset(state.guesses, 0, sizeof(state.guesses));
    memset(&state.secret, 0, sizeof(state.secret));
    state.roundWinnerBoardId = 0;
    state.guessCount = 0;
    ++state.round;
    ++state.revision;
    state.phase = MastermindPhase::SecretEntry;
    return true;
}

// Transitions to Exited, clearing sensitive round data. Either player may exit.
inline bool exitMastermindMatch(MastermindState& state,
                                uint32_t actorBoardId) {
    if (state.phase == MastermindPhase::Exited ||
        (actorBoardId != state.hostBoardId &&
         actorBoardId != state.guestBoardId)) {
        return false;
    }
    memset(state.guesses, 0, sizeof(state.guesses));
    memset(&state.secret, 0, sizeof(state.secret));
    state.roundWinnerBoardId = 0;
    state.guessCount = 0;
    ++state.revision;
    state.phase = MastermindPhase::Exited;
    return true;
}

// Ordering result for comparing two Mastermind state snapshots.
enum class MastermindVersionOrder : uint8_t { Older, Same, Newer };

// Compares by epoch (gameId) then revision to determine which state is authoritative.
inline MastermindVersionOrder compareMastermindVersion(
    const MastermindState& current, const MastermindState& candidate) {
    if (candidate.gameId != current.gameId) {
        return candidate.gameId > current.gameId
                   ? MastermindVersionOrder::Newer
                   : MastermindVersionOrder::Older;
    }
    if (candidate.revision != current.revision) {
        return candidate.revision > current.revision
                   ? MastermindVersionOrder::Newer
                   : MastermindVersionOrder::Older;
    }
    return MastermindVersionOrder::Same;
}

// Computes a FNV-1a digest over the entire state for integrity and deduplication.
inline uint32_t mastermindStateDigest(const MastermindState& state) {
    uint32_t digest = 2166136261u;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
    for (size_t index = 0; index < sizeof(state); ++index) {
        digest ^= bytes[index];
        digest *= 16777619u;
    }
    return digest;
}

// Returns true when the candidate is valid and newer; used to accept incoming
// full-state packets.
inline bool shouldAdoptMastermindState(const MastermindState& current,
                                       const MastermindState& candidate) {
    return isValidMastermindState(candidate) &&
           compareMastermindVersion(current, candidate) ==
               MastermindVersionOrder::Newer;
}

// Returns true when the observed state makes a pending delivery obsolete.
inline bool isMastermindDeliverySuperseded(
    const MastermindState& pending, const MastermindState& observed) {
    return pending.gameId == observed.gameId &&
           pending.revision < observed.revision;
}

// Applies an exit signal from a peer; idempotent if already exited.
inline bool applyMastermindExitSignal(
    MastermindState& current, const MastermindState& candidate,
    uint32_t senderBoardId) {
    if (!isValidMastermindState(current) ||
        !isValidMastermindState(candidate) ||
        candidate.phase != MastermindPhase::Exited ||
        current.gameId != candidate.gameId ||
        current.hostBoardId != candidate.hostBoardId ||
        current.guestBoardId != candidate.guestBoardId) {
        return false;
    }
    if (current.phase == MastermindPhase::Exited) {
        return senderBoardId == current.hostBoardId ||
               senderBoardId == current.guestBoardId;
    }
    return exitMastermindMatch(current, senderBoardId);
}

// Verifies that the candidate is a single legal successor by replaying the
// implied action and comparing the result byte-for-byte.
inline bool isValidMastermindTransition(const MastermindState& current,
                                        const MastermindState& candidate,
                                        uint32_t senderBoardId) {
    if (!isValidMastermindState(current) ||
        !isValidMastermindState(candidate) ||
        candidate.gameId != current.gameId ||
        candidate.revision != current.revision + 1) {
        return false;
    }
    MastermindState expected = current;
    bool applied = false;
    if (candidate.phase == MastermindPhase::Exited) {
        applied = exitMastermindMatch(expected, senderBoardId);
    } else if (current.phase == MastermindPhase::SecretEntry) {
        applied = submitMastermindSecret(expected, candidate.secret,
                                         senderBoardId);
    } else if (current.phase == MastermindPhase::Guessing &&
               current.guessCount < kMastermindMaxGuesses) {
        applied = submitMastermindGuess(
            expected, candidate.guesses[current.guessCount].code,
            senderBoardId);
    } else if (current.phase == MastermindPhase::RoundComplete) {
        applied = advanceMastermindRound(expected, senderBoardId);
    }
    return applied && sameMastermindState(expected, candidate);
}
