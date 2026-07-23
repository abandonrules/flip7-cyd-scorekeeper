#include <cassert>
#include <cstdint>

#include "protocol.h"

static_assert(sizeof(GameStatePacket) == 116);
static_assert(sizeof(GameAckPacket) == 40);

int main() {
    const MastermindCode duplicateSecret{{1, 1, 2, 3}};
    const MastermindCode duplicateGuess{{1, 2, 1, 4}};
    const MastermindFeedback duplicateFeedback =
        evaluateMastermindGuess(duplicateSecret, duplicateGuess);
    assert(duplicateFeedback.exact == 1);
    assert(duplicateFeedback.colorOnly == 2);

    const MastermindCode crossedSecret{{1, 1, 2, 2}};
    const MastermindCode crossedGuess{{1, 2, 1, 2}};
    const MastermindFeedback crossedFeedback =
        evaluateMastermindGuess(crossedSecret, crossedGuess);
    assert(crossedFeedback.exact == 2);
    assert(crossedFeedback.colorOnly == 2);

    MastermindState game = makeMastermindMatch(10, 20, 7);
    assert(isValidMastermindState(game));
    MastermindState malformed = game;
    malformed.guesses[9].code.colors[0] = 1;
    assert(!isValidMastermindState(malformed));
    malformed = game;
    malformed.reserved[0] = 1;
    assert(!isValidMastermindState(malformed));
    malformed = game;
    malformed.hostScore = 1;
    assert(!isValidMastermindState(malformed));
    assert(game.gameId == 7);
    assert(game.round == 1);
    assert(game.phase == MastermindPhase::SecretEntry);
    assert(game.codemakerBoardId == 10);
    assert(game.hostScore == 0 && game.guestScore == 0);
    MastermindState exited = game;
    assert(exitMastermindMatch(exited, 20));
    assert(exited.phase == MastermindPhase::Exited);
    assert(exited.revision == 1);
    assert(isValidMastermindState(exited));
    assert(isValidMastermindTransition(game, exited, 20));
    assert(!exitMastermindMatch(exited, 10));

    const MastermindCode secret{{1, 2, 3, 4}};
    assert(!submitMastermindSecret(game, secret, 20));
    MastermindCode invalidSecret{{1, 2, 3, 7}};
    assert(!submitMastermindSecret(game, invalidSecret, 10));
    assert(submitMastermindSecret(game, secret, 10));
    assert(game.phase == MastermindPhase::Guessing);
    assert(game.revision == 1);

    const MastermindCode miss{{1, 2, 4, 3}};
    assert(!submitMastermindGuess(game, miss, 10));
    assert(submitMastermindGuess(game, miss, 20));
    assert(game.guessCount == 1);
    assert(game.guesses[0].feedback.exact == 2);
    assert(game.guesses[0].feedback.colorOnly == 2);
    assert(game.phase == MastermindPhase::Guessing);

    assert(submitMastermindGuess(game, secret, 20));
    assert(game.phase == MastermindPhase::RoundComplete);
    assert(game.roundWinnerBoardId == 20);
    assert(game.guestScore == 1);
    assert(game.revision == 3);
    const MastermindState completedRound = game;

    assert(!advanceMastermindRound(game, 20));
    assert(advanceMastermindRound(game, 10));
    assert(game.phase == MastermindPhase::SecretEntry);
    assert(game.codemakerBoardId == 20);
    assert(game.round == 2);
    assert(game.guessCount == 0);
    assert(game.secret.colors[0] == 0);
    assert(game.revision == 4);

    MastermindState exhausted = makeMastermindMatch(10, 20, 8);
    assert(submitMastermindSecret(exhausted, secret, 10));
    const MastermindCode noMatch{{5, 5, 5, 5}};
    for (uint8_t guess = 0; guess < kMastermindMaxGuesses; ++guess) {
        assert(submitMastermindGuess(exhausted, noMatch, 20));
    }
    assert(exhausted.phase == MastermindPhase::RoundComplete);
    assert(exhausted.roundWinnerBoardId == 10);
    assert(exhausted.hostScore == 1);

    MastermindState beforeGuess = makeMastermindMatch(10, 20, 9);
    assert(submitMastermindSecret(beforeGuess, secret, 10));
    MastermindState afterGuess = beforeGuess;
    assert(submitMastermindGuess(afterGuess, miss, 20));
    assert(isValidMastermindTransition(beforeGuess, afterGuess, 20));
    assert(!isValidMastermindTransition(beforeGuess, afterGuess, 10));
    MastermindState invalidWinner = completedRound;
    invalidWinner.roundWinnerBoardId = 999;
    assert(!isValidMastermindState(invalidWinner));
    invalidWinner.roundWinnerBoardId = invalidWinner.codemakerBoardId;
    assert(!isValidMastermindState(invalidWinner));

    const uint32_t digest = mastermindStateDigest(beforeGuess);
    assert(digest != mastermindStateDigest(afterGuess));
    assert(compareMastermindVersion(beforeGuess, afterGuess) ==
           MastermindVersionOrder::Newer);
    assert(shouldAdoptMastermindState(beforeGuess, afterGuess));
    assert(!shouldAdoptMastermindState(afterGuess, beforeGuess));
    assert(isMastermindDeliverySuperseded(beforeGuess, afterGuess));

    return 0;
}
