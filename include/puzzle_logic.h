#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint8_t kPuzzleColumns = 4;
constexpr uint8_t kPuzzleRows = 3;
constexpr uint8_t kPuzzleTileCount = kPuzzleColumns * kPuzzleRows;

struct PuzzleState {
    uint8_t tiles[kPuzzleTileCount];
    uint32_t revision;
    uint32_t turnBoardId;
};

inline PuzzleState makeInitialPuzzle(uint32_t firstTurnBoardId) {
    PuzzleState state{};
    for (uint8_t i = 0; i + 1 < kPuzzleTileCount; ++i) {
        state.tiles[i] = i + 1;
    }
    state.revision = 0;
    state.turnBoardId = firstTurnBoardId;
    return state;
}

inline bool isValidPuzzle(const PuzzleState& state) {
    bool seen[kPuzzleTileCount]{};
    for (uint8_t tile : state.tiles) {
        if (tile >= kPuzzleTileCount || seen[tile]) {
            return false;
        }
        seen[tile] = true;
    }
    return state.turnBoardId != 0;
}

inline uint8_t findBlank(const PuzzleState& state) {
    for (uint8_t i = 0; i < kPuzzleTileCount; ++i) {
        if (state.tiles[i] == 0) {
            return i;
        }
    }
    return kPuzzleTileCount;
}

inline bool arePuzzlePositionsAdjacent(uint8_t first, uint8_t second) {
    if (first >= kPuzzleTileCount || second >= kPuzzleTileCount) {
        return false;
    }
    const uint8_t firstRow = first / kPuzzleColumns;
    const uint8_t firstColumn = first % kPuzzleColumns;
    const uint8_t secondRow = second / kPuzzleColumns;
    const uint8_t secondColumn = second % kPuzzleColumns;
    const uint8_t rowDistance = firstRow > secondRow ? firstRow - secondRow
                                                      : secondRow - firstRow;
    const uint8_t columnDistance = firstColumn > secondColumn
                                       ? firstColumn - secondColumn
                                       : secondColumn - firstColumn;
    return rowDistance + columnDistance == 1;
}

inline bool tryPuzzleMove(PuzzleState& state, uint8_t position,
                          uint32_t actorBoardId, uint32_t nextTurnBoardId) {
    if (state.turnBoardId != actorBoardId || actorBoardId == nextTurnBoardId ||
        position >= kPuzzleTileCount || state.tiles[position] == 0) {
        return false;
    }
    const uint8_t blank = findBlank(state);
    if (!arePuzzlePositionsAdjacent(position, blank)) {
        return false;
    }
    state.tiles[blank] = state.tiles[position];
    state.tiles[position] = 0;
    ++state.revision;
    state.turnBoardId = nextTurnBoardId;
    return true;
}

inline bool isValidRemoteTransition(const PuzzleState& current,
                                    const PuzzleState& incoming,
                                    uint32_t senderBoardId,
                                    uint32_t recipientBoardId) {
    if (!isValidPuzzle(current) || !isValidPuzzle(incoming) ||
        current.turnBoardId != senderBoardId ||
        incoming.turnBoardId != recipientBoardId ||
        incoming.revision != current.revision + 1) {
        return false;
    }

    const uint8_t oldBlank = findBlank(current);
    const uint8_t newBlank = findBlank(incoming);
    if (!arePuzzlePositionsAdjacent(oldBlank, newBlank) ||
        incoming.tiles[oldBlank] != current.tiles[newBlank]) {
        return false;
    }

    for (uint8_t i = 0; i < kPuzzleTileCount; ++i) {
        if (i != oldBlank && i != newBlank &&
            incoming.tiles[i] != current.tiles[i]) {
            return false;
        }
    }
    return true;
}
