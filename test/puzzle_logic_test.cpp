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

    const PuzzleSpec planets{3, 3, PuzzleTheme::Planets};
    const PuzzleSpec greek{4, 3, PuzzleTheme::Greek};
    assert(isSupportedPuzzleSpec(planets));
    assert(isSupportedPuzzleSpec(greek));
    assert(!isSupportedPuzzleSpec({3, 3, PuzzleTheme::Greek}));

    PuzzleState state = makeInitialPuzzle(10, 7, planets);
    assert(state.gameId == 7);
    assert(state.revision == 0);
    assert(state.turnBoardId == 10);
    assert(puzzleTileCount(state) == 9);
    assert(state.theme == PuzzleTheme::Planets);
    for (uint8_t i = 0; i < 8; ++i) {
        assert(state.tiles[i] == i + 1);
    }
    assert(state.tiles[8] == 0);
    for (uint8_t i = 9; i < kPuzzleMaxTiles; ++i) {
        assert(state.tiles[i] == 0);
    }
    assert(isValidPuzzle(state));
    assert(isPuzzleSolved(state));

    PuzzleState greekState = makeInitialPuzzle(10, 8, greek);
    assert(puzzleTileCount(greekState) == 12);
    assert(greekState.theme == PuzzleTheme::Greek);
    assert(isValidPuzzle(greekState));
    assert(isPuzzleSolved(greekState));

    PuzzleState malformed = state;
    malformed.columns = 4;
    assert(!isValidPuzzle(malformed));
    malformed = state;
    malformed.tiles[9] = 1;
    assert(!isValidPuzzle(malformed));
    malformed = state;
    malformed.theme = static_cast<PuzzleTheme>(99);
    assert(!isValidPuzzle(malformed));
    malformed = state;
    malformed.phase = static_cast<PuzzlePhase>(99);
    assert(!isValidPuzzle(malformed));

    PuzzleState scrambled = makeScrambledPuzzle(10, 9, planets);
    assert(isValidPuzzle(scrambled));
    assert(!isPuzzleSolved(scrambled));
    assert(!isPuzzleSolved(makeScrambledPuzzle(0x6AF4E9D4, 731, planets)));
    PuzzleState unsolvable = state;
    const uint8_t firstUnsolvable = unsolvable.tiles[0];
    unsolvable.tiles[0] = unsolvable.tiles[1];
    unsolvable.tiles[1] = firstUnsolvable;
    assert(!isValidPuzzle(unsolvable));
    assert(isSamePuzzle(scrambled,
                        makeScrambledPuzzle(10, 9, planets)));

    PuzzleState greekScrambled = makeScrambledPuzzle(10, 10, greek);
    assert(isValidPuzzle(greekScrambled));
    assert(!isPuzzleSolved(greekScrambled));

    const uint8_t blank = findBlank(scrambled);
    uint8_t movable = kPuzzleMaxTiles;
    for (uint8_t position = 0; position < puzzleTileCount(scrambled);
         ++position) {
        if (arePuzzlePositionsAdjacent(scrambled, position, blank)) {
            movable = position;
            break;
        }
    }
    assert(movable < puzzleTileCount(scrambled));
    PuzzleState remote = scrambled;
    assert(tryPuzzleMove(remote, movable, 10, 20));
    assert(remote.revision == 1);
    assert(remote.turnBoardId == 20);
    assert(isValidRemoteTransition(scrambled, remote, 10, 20));
    assert(!isValidRemoteTransition(scrambled, remote, 20, 10));

    PuzzleState wrongSpec = remote;
    wrongSpec.theme = PuzzleTheme::Greek;
    assert(!isValidRemoteTransition(scrambled, wrongSpec, 10, 20));

    PuzzleState exhausted = scrambled;
    exhausted.revision = UINT32_MAX;
    assert(!tryPuzzleMove(exhausted, movable, 10, 20));
    assert(!exitPuzzle(exhausted, 10, 20));

    PuzzleState exited = scrambled;
    assert(exitPuzzle(exited, 20, 10));
    assert(exited.phase == PuzzlePhase::Exited);
    assert(exited.revision == scrambled.revision + 1);
    assert(!tryPuzzleMove(exited, movable, 10, 20));
    PuzzleState receiver = scrambled;
    PuzzleState revisionGapExit = exited;
    revisionGapExit.revision += 2;
    assert(applyPuzzleExitSignal(receiver, revisionGapExit, 20, 10));
    assert(isSamePuzzle(receiver, revisionGapExit));
    const PuzzleState exitedReceiver = receiver;
    assert(applyPuzzleExitSignal(receiver, exited, 20, 10));
    assert(isSamePuzzle(receiver, exitedReceiver));
    PuzzleState otherGameExit = exited;
    ++otherGameExit.gameId;
    assert(!applyPuzzleExitSignal(receiver, otherGameExit, 20, 10));

    PuzzleState newerRevision = scrambled;
    newerRevision.revision = 3;
    newerRevision.turnBoardId = 20;
    assert(shouldAdoptFullState(scrambled, newerRevision, 10, 20));
    assert(!shouldAdoptFullState(newerRevision, scrambled, 20, 10));
    assert(!shouldAdoptFullState(scrambled, scrambled, 10, 20));
    PuzzleState newerGame = makeScrambledPuzzle(10, 99, planets);
    assert(shouldAdoptFullState(scrambled, newerGame, 10, 20));
    assert(!shouldAdoptFullState(scrambled, newerGame, 20, 10));
    assert(!shouldAdoptFullState(scrambled, unsolvable, 10, 20));

    const uint32_t digest = puzzleStateDigest(scrambled);
    PuzzleState divergent = scrambled;
    const uint8_t first = divergent.tiles[0];
    divergent.tiles[0] = divergent.tiles[1];
    divergent.tiles[1] = first;
    assert(puzzleStateDigest(divergent) != digest);
    divergent = scrambled;
    divergent.theme = PuzzleTheme::Greek;
    assert(puzzleStateDigest(divergent) != digest);

    assert(isDeliverySuperseded(scrambled, newerRevision));
    assert(!isDeliverySuperseded(newerRevision, scrambled));
    assert(decideReconciliation(scrambled, scrambled.gameId,
                                scrambled.revision, digest, true) ==
           ReconciliationAction::None);

    return 0;
}
