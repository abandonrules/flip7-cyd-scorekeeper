#include <cassert>
#include <cstdint>

#include "protocol.h"

int main() {
    assert(isSequenceNewer(1, 0));
    assert(!isSequenceNewer(1, 1));
    assert(!isSequenceNewer(1, 2));
    assert(isSequenceNewer(0, UINT32_MAX));

    const uint32_t retiredSessions[] = {11, 22};
    assert(canUsePeerSession(0, 33, false, retiredSessions, 2));
    assert(canUsePeerSession(33, 33, false, retiredSessions, 2));
    assert(!canUsePeerSession(33, 44, false, retiredSessions, 2));
    assert(!canUsePeerSession(33, 22, true, retiredSessions, 2));
    assert(canUsePeerSession(33, 44, true, retiredSessions, 2));

    PuzzleState state = makeInitialPuzzle(10, 7);
    assert(state.gameId == 7);
    assert(state.revision == 0);
    assert(state.turnBoardId == 10);
    for (uint8_t i = 0; i < 11; ++i) {
        assert(state.tiles[i] == i + 1);
    }
    assert(state.tiles[11] == 0);

    assert(!tryPuzzleMove(state, 0, 10, 20));
    assert(state.revision == 0);
    assert(!tryPuzzleMove(state, 11, 10, 20));
    assert(state.revision == 0);

    assert(isTileCorrect(state, 10));
    assert(tryPuzzleMove(state, 10, 10, 20));
    assert(state.tiles[10] == 0);
    assert(state.tiles[11] == 11);
    assert(state.revision == 1);
    assert(state.turnBoardId == 20);

    assert(!tryPuzzleMove(state, 9, 10, 20));
    assert(state.revision == 1);
    assert(tryPuzzleMove(state, 9, 20, 10));
    assert(state.revision == 2);
    assert(state.turnBoardId == 10);

    PuzzleState remote = state;
    assert(tryPuzzleMove(remote, 8, 10, 20));
    assert(isValidRemoteTransition(state, remote, 10, 20));
    assert(!isValidRemoteTransition(state, remote, 20, 10));

    PuzzleState duplicate = state;
    assert(!isValidRemoteTransition(state, duplicate, 10, 20));

    PuzzleState invalid = remote;
    invalid.tiles[0] = invalid.tiles[1];
    assert(!isValidPuzzle(invalid));
    assert(!isValidRemoteTransition(state, invalid, 10, 20));

    PuzzleState scrambled = makeScrambledPuzzle(10, 8);
    assert(scrambled.gameId == 8);
    assert(scrambled.revision == 0);
    assert(scrambled.turnBoardId == 10);
    assert(isValidPuzzle(scrambled));
    assert(!isPuzzleSolved(scrambled));

    PuzzleState solution = scrambled;
    constexpr uint8_t solveMoves[] = {9, 10, 11, 7, 6, 2, 1, 0,
                                      4, 8,  9,  5, 6, 10, 11};
    for (uint8_t position : solveMoves) {
        const uint32_t actor = solution.turnBoardId;
        const uint32_t next = actor == 10 ? 20 : 10;
        assert(tryPuzzleMove(solution, position, actor, next));
    }
    assert(isPuzzleSolved(solution));

    PuzzleState solved = makeInitialPuzzle(10, 9);
    assert(isPuzzleSolved(solved));
    for (uint8_t position = 0; position < 11; ++position) {
        assert(isTileCorrect(solved, position));
    }
    assert(!isTileCorrect(solved, 11));
    assert(!isTileCorrect(scrambled, findBlank(scrambled)));

    PuzzleState newerGame = makeScrambledPuzzle(20, 10);
    assert(shouldAdoptFullState(solved, newerGame));
    assert(!shouldAdoptFullState(newerGame, solved));
    PuzzleState newerRevision = scrambled;
    newerRevision.revision = 3;
    assert(shouldAdoptFullState(scrambled, newerRevision));
    assert(!shouldAdoptFullState(newerRevision, scrambled));
    assert(!shouldAdoptFullState(scrambled, scrambled));
    newerGame.revision = 0;
    assert(shouldAdoptFullState(solved, newerGame));

    assert(isSamePuzzle(scrambled, scrambled));
    assert(!isSamePuzzle(scrambled, newerRevision));
    assert(comparePuzzleVersion(scrambled, newerRevision) ==
           PuzzleVersionOrder::Newer);
    assert(comparePuzzleVersion(newerRevision, scrambled) ==
           PuzzleVersionOrder::Older);
    assert(comparePuzzleVersion(solved, solved) == PuzzleVersionOrder::Same);
    const uint32_t solvedDigest = puzzleStateDigest(solved);
    PuzzleState divergent = solved;
    const uint8_t first = divergent.tiles[0];
    divergent.tiles[0] = divergent.tiles[1];
    divergent.tiles[1] = first;
    assert(puzzleStateDigest(divergent) != solvedDigest);

    assert(isDeliverySuperseded(scrambled, newerRevision));
    assert(!isDeliverySuperseded(newerRevision, scrambled));
    assert(decideReconciliation(solved, solved.gameId, solved.revision,
                                solvedDigest, true) ==
           ReconciliationAction::None);
    assert(decideReconciliation(newerRevision, newerRevision.gameId,
                                newerRevision.revision - 1, 0, false) ==
           ReconciliationAction::SendFullState);
    assert(decideReconciliation(solved, solved.gameId, solved.revision + 1, 0,
                                true) ==
           ReconciliationAction::RequestFullState);
    assert(decideReconciliation(solved, solved.gameId, solved.revision,
                                puzzleStateDigest(divergent), true) ==
           ReconciliationAction::SendFullState);
    assert(decideReconciliation(solved, solved.gameId, solved.revision,
                                puzzleStateDigest(divergent), false) ==
           ReconciliationAction::RequestFullState);

    return 0;
}
