#include <cassert>
#include <cstdint>

#include "puzzle_logic.h"

int main() {
    PuzzleState state = makeInitialPuzzle(10);
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

    return 0;
}
