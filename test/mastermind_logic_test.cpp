#include <cassert>
#include <cstdint>

#include "game_selection.h"
#include "protocol.h"

static_assert(sizeof(GameStatePacket) == 116);
static_assert(sizeof(GameAckPacket) == 40);

int main() {
    const ActiveGameClock homeClock{7, ActiveGameKind::Home};
    assert(shouldAcceptGameState(homeClock, ActiveGameKind::Mastermind, 8));
    assert(!shouldAcceptGameState(homeClock, ActiveGameKind::Puzzle, 6));
    assert(!shouldAcceptGameState(homeClock, ActiveGameKind::Puzzle, 7));
    assert(shouldAcceptGameState(homeClock, ActiveGameKind::Mastermind, 7,
                                 true));
    const ActiveGameClock mastermindClock{8, ActiveGameKind::Mastermind};
    assert(shouldAcceptGameState(mastermindClock,
                                 ActiveGameKind::Mastermind, 8));
    assert(!shouldAcceptGameState(mastermindClock, ActiveGameKind::Puzzle, 8));
    assert(nextActiveGameEpoch(7, 3, 8) == 9);
    assert(nextActiveGameEpoch(UINT32_MAX, 3, 8) ==
           kNoActiveGameEpoch);
    assert(!canReturnCompletedGameHome(mastermindClock,
                                       ActiveGameKind::Puzzle, false));
    const ActiveGameClock puzzleClock{9, ActiveGameKind::Puzzle};
    assert(!canReturnCompletedGameHome(puzzleClock,
                                       ActiveGameKind::Puzzle, true));
    assert(canReturnCompletedGameHome(puzzleClock,
                                      ActiveGameKind::Puzzle, false));
    assert(shouldSendActiveGameState(puzzleClock,
                                     ActiveGameKind::Puzzle, 9, true));
    assert(!shouldSendActiveGameState(homeClock,
                                      ActiveGameKind::Puzzle, 7, true));
    assert(shouldSendActiveGameState(homeClock,
                                     ActiveGameKind::Puzzle, 7, true, true));
    assert(!shouldSendActiveGameState(puzzleClock,
                                      ActiveGameKind::Puzzle, 8, true));
    assert(!shouldSendActiveGameState(puzzleClock,
                                      ActiveGameKind::Puzzle, 9, false));

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

    MastermindState peerAhead = game;
    const MastermindCode peerSecret{{1, 2, 3, 4}};
    assert(submitMastermindSecret(peerAhead, peerSecret, 10));
    assert(exitMastermindMatch(peerAhead, 20));
    MastermindState receiverBehind = game;
    assert(applyMastermindExitSignal(receiverBehind, peerAhead, 20));
    assert(receiverBehind.phase == MastermindPhase::Exited);
    assert(receiverBehind.revision == 1);
    const MastermindState exitedReceiverSnapshot = receiverBehind;
    assert(applyMastermindExitSignal(receiverBehind, peerAhead, 20));
    assert(sameMastermindState(receiverBehind, exitedReceiverSnapshot));

    MastermindState simultaneousHostAction = game;
    assert(submitMastermindSecret(simultaneousHostAction, peerSecret, 10));
    MastermindState simultaneousGuestExit = game;
    assert(exitMastermindMatch(simultaneousGuestExit, 20));
    assert(applyMastermindExitSignal(simultaneousHostAction,
                                    simultaneousGuestExit, 20));
    assert(simultaneousHostAction.phase == MastermindPhase::Exited);

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
    MastermindState wrongRoundTwoMaker = game;
    wrongRoundTwoMaker.codemakerBoardId = wrongRoundTwoMaker.hostBoardId;
    assert(!isValidMastermindState(wrongRoundTwoMaker));
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

    MastermindState finalRound = completedRound;
    finalRound.round = UINT8_MAX;
    finalRound.hostScore = UINT8_MAX - 1;
    finalRound.guestScore = 1;
    assert(isValidMastermindState(finalRound));
    assert(!advanceMastermindRound(finalRound, finalRound.hostBoardId));

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
