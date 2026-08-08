/**
 * Flip7 Core - Mastermind Game Logic
 * Platform-neutral Mastermind logic
 */

#include "flip7/protocol.hpp"
#include <cstring>

namespace flip7 {

// Helper: check if code is empty (all zeros)
bool isEmptyMastermindCode(const MastermindCode& code) {
    for (uint8_t color : code.colors) {
        if (color != 0) return false;
    }
    return true;
}

// Helper: validate code (colors 1-7)
bool isValidMastermindCode(const MastermindCode& code) {
    for (uint8_t color : code.colors) {
        if (color == 0 || color > kMastermindColorCount) return false;
    }
    return true;
}

// Evaluate guess against secret
MastermindFeedback evaluateMastermindGuess(const MastermindCode& secret, const MastermindCode& guess) {
    MastermindFeedback feedback{};
    uint8_t secretCounts[kMastermindColorCount + 1]{};
    uint8_t guessCounts[kMastermindColorCount + 1]{};
    
    for (uint8_t i = 0; i < kMastermindCodeLength; ++i) {
        if (secret.colors[i] == guess.colors[i]) {
            ++feedback.exact;
        } else {
            ++secretCounts[secret.colors[i]];
            ++guessCounts[guess.colors[i]];
        }
    }
    for (uint8_t color = 1; color <= kMastermindColorCount; ++color) {
        feedback.colorOnly += secretCounts[color] < guessCounts[color] ? secretCounts[color] : guessCounts[color];
    }
    return feedback;
}

// Get codebreaker board ID (the one who is NOT the codemaker)
uint32_t mastermindCodebreakerBoardId(const MastermindState& state) {
    return state.codemakerBoardId == state.hostBoardId ? state.guestBoardId : state.hostBoardId;
}

// Create new match
MastermindState makeMastermindMatch(uint32_t hostBoardId, uint32_t guestBoardId, uint32_t gameId) {
    MastermindState state{};
    state.gameId = gameId;
    state.hostBoardId = hostBoardId;
    state.guestBoardId = guestBoardId;
    state.codemakerBoardId = hostBoardId;
    state.round = 1;
    state.phase = MastermindPhase::Setup;
    return state;
}

// Compare two codes
bool sameMastermindCode(const MastermindCode& first, const MastermindCode& second) {
    return memcmp(first.colors, second.colors, kMastermindCodeLength) == 0;
}

// Compare two full states
bool sameMastermindState(const MastermindState& first, const MastermindState& second) {
    return memcmp(&first, &second, sizeof(MastermindState)) == 0;
}

// Full validation
bool isValidMastermindState(const MastermindState& state) {
    if (state.gameId == 0 || state.hostBoardId == 0 ||
        state.guestBoardId == 0 ||
        state.hostBoardId == state.guestBoardId ||
        (state.codemakerBoardId != state.hostBoardId &&
         state.codemakerBoardId != state.guestBoardId) ||
        state.round == 0 ||
        state.codemakerBoardId !=
            (state.round % 2 == 1 ? state.hostBoardId : state.guestBoardId) ||
        state.guessCount > kMastermindMaxGuesses ||
        state.phase < MastermindPhase::Setup ||
        state.phase > MastermindPhase::Exited) {
        return false;
    }
    
    uint16_t scoreTotal = static_cast<uint16_t>(state.hostScore) + state.guestScore;
    uint16_t completedRounds = static_cast<uint16_t>(state.round - 1) +
                               (state.phase == MastermindPhase::RoundComplete ? 1 : 0);
    bool validExitedScore = state.phase == MastermindPhase::Exited &&
                            (scoreTotal == state.round - 1 || scoreTotal == state.round);
    if (scoreTotal != completedRounds && !validExitedScore) return false;
    
    for (uint8_t value : state.reserved) {
        if (value != 0) return false;
    }
    
    MastermindGuess emptyGuess{};
    for (uint8_t i = state.guessCount; i < kMastermindMaxGuesses; ++i) {
        if (memcmp(&state.guesses[i], &emptyGuess, sizeof(emptyGuess)) != 0) return false;
    }
    
    if (state.phase == MastermindPhase::Setup || state.phase == MastermindPhase::Exited) {
        return state.guessCount == 0 && state.roundWinnerBoardId == 0 &&
               isEmptyMastermindCode(state.secret);
    }
    
    if (!isValidMastermindCode(state.secret) ||
        (state.guessCount == 0 && state.phase == MastermindPhase::RoundComplete)) {
        return false;
    }
    
    for (uint8_t i = 0; i < state.guessCount; ++i) {
        if (!isValidMastermindCode(state.guesses[i].code)) return false;
        MastermindFeedback expected = evaluateMastermindGuess(state.secret, state.guesses[i].code);
        if (state.guesses[i].feedback.exact != expected.exact ||
            state.guesses[i].feedback.colorOnly != expected.colorOnly) {
            return false;
        }
    }
    
    if (state.phase == MastermindPhase::Playing) {
        return state.guessCount < kMastermindMaxGuesses &&
               state.roundWinnerBoardId == 0 &&
               (state.guessCount == 0 ||
                state.guesses[state.guessCount - 1].feedback.exact < kMastermindCodeLength);
    }
    
    bool solved = state.guesses[state.guessCount - 1].feedback.exact == kMastermindCodeLength;
    uint32_t expectedWinner = solved ? mastermindCodebreakerBoardId(state) : state.codemakerBoardId;
    return state.roundWinnerBoardId == expectedWinner &&
           (solved || state.guessCount == kMastermindMaxGuesses);
}

// Submit secret (codemaker sets the code)
bool submitMastermindSecret(MastermindState& state, const MastermindCode& secret, uint32_t actorBoardId) {
    if (state.phase != MastermindPhase::Setup ||
        actorBoardId != state.codemakerBoardId ||
        !isValidMastermindCode(secret)) {
        return false;
    }
    state.secret = secret;
    state.phase = MastermindPhase::Playing;
    ++state.revision;
    return true;
}

// Submit guess (codebreaker makes a guess)
bool submitMastermindGuess(MastermindState& state, const MastermindCode& guess, uint32_t actorBoardId) {
    if (state.phase != MastermindPhase::Playing ||
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

// Advance to next round
bool advanceMastermindRound(MastermindState& state, uint32_t actorBoardId) {
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
    state.phase = MastermindPhase::Setup;
    return true;
}

// Exit match
bool exitMastermindMatch(MastermindState& state, uint32_t actorBoardId) {
    if (state.phase == MastermindPhase::Exited ||
        (actorBoardId != state.hostBoardId && actorBoardId != state.guestBoardId)) {
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

// Version comparison
MastermindVersionOrder compareMastermindVersion(const MastermindState& current, const MastermindState& candidate) {
    if (candidate.gameId != current.gameId) {
        return candidate.gameId > current.gameId ? MastermindVersionOrder::Newer : MastermindVersionOrder::Older;
    }
    if (candidate.revision != current.revision) {
        return candidate.revision > current.revision ? MastermindVersionOrder::Newer : MastermindVersionOrder::Older;
    }
    return MastermindVersionOrder::Same;
}

// FNV-1a digest
uint32_t mastermindStateDigest(const MastermindState& state) {
    uint32_t digest = 2166136261u;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
    for (size_t i = 0; i < sizeof(state); ++i) {
        digest ^= bytes[i];
        digest *= 16777619u;
    }
    return digest;
}

// Should adopt newer state
bool shouldAdoptMastermindState(const MastermindState& current, const MastermindState& candidate) {
    return isValidMastermindState(candidate) &&
           compareMastermindVersion(current, candidate) == MastermindVersionOrder::Newer;
}

// Check if delivery superseded
bool isMastermindDeliverySuperseded(const MastermindState& pending, const MastermindState& observed) {
    return pending.gameId == observed.gameId && pending.revision < observed.revision;
}

// Apply exit signal from peer
bool applyMastermindExitSignal(MastermindState& current, const MastermindState& candidate, uint32_t senderBoardId) {
    if (!isValidMastermindState(current) || !isValidMastermindState(candidate) ||
        candidate.phase != MastermindPhase::Exited ||
        current.gameId != candidate.gameId ||
        current.hostBoardId != candidate.hostBoardId ||
        current.guestBoardId != candidate.guestBoardId) {
        return false;
    }
    if (current.phase == MastermindPhase::Exited) {
        return senderBoardId == current.hostBoardId || senderBoardId == current.guestBoardId;
    }
    return exitMastermindMatch(current, senderBoardId);
}

// Validate transition from current to candidate
bool isValidMastermindTransition(const MastermindState& current, const MastermindState& candidate, uint32_t senderBoardId) {
    if (!isValidMastermindState(current) || !isValidMastermindState(candidate) ||
        candidate.gameId != current.gameId ||
        candidate.revision != current.revision + 1) {
        return false;
    }
    
    MastermindState expected = current;
    bool applied = false;
    
    if (candidate.phase == MastermindPhase::Exited) {
        applied = exitMastermindMatch(expected, senderBoardId);
    } else if (current.phase == MastermindPhase::Setup) {
        applied = submitMastermindSecret(expected, candidate.secret, senderBoardId);
    } else if (current.phase == MastermindPhase::Playing && current.guessCount < kMastermindMaxGuesses) {
        applied = submitMastermindGuess(expected, candidate.guesses[current.guessCount].code, senderBoardId);
    } else if (current.phase == MastermindPhase::RoundComplete) {
        applied = advanceMastermindRound(expected, senderBoardId);
    }
    
    return applied && sameMastermindState(expected, candidate);
}

} // namespace flip7