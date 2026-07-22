#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint8_t kPuzzleColumns = 4;
constexpr uint8_t kPuzzleRows = 3;
constexpr uint8_t kPuzzleTileCount = kPuzzleColumns * kPuzzleRows;

struct PuzzleState {
    uint8_t tiles[kPuzzleTileCount];
    uint32_t gameId;
    uint32_t revision;
    uint32_t turnBoardId;
};

inline PuzzleState makeInitialPuzzle(uint32_t firstTurnBoardId,
                                     uint32_t gameId) {
    PuzzleState state{};
    for (uint8_t i = 0; i + 1 < kPuzzleTileCount; ++i) {
        state.tiles[i] = i + 1;
    }
    state.gameId = gameId;
    state.revision = 0;
    state.turnBoardId = firstTurnBoardId;
    return state;
}

inline PuzzleState makeScrambledPuzzle(uint32_t firstTurnBoardId,
                                       uint32_t gameId) {
    PuzzleState state = makeInitialPuzzle(firstTurnBoardId, gameId);
    constexpr uint8_t moves[] = {10, 6, 5, 9, 8, 4, 0, 1,
                                 2,  6, 7, 11, 10, 9, 5};
    uint8_t blank = kPuzzleTileCount - 1;
    for (uint8_t position : moves) {
        state.tiles[blank] = state.tiles[position];
        state.tiles[position] = 0;
        blank = position;
    }
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
    return state.gameId != 0 && state.turnBoardId != 0;
}

inline uint8_t findBlank(const PuzzleState& state) {
    for (uint8_t i = 0; i < kPuzzleTileCount; ++i) {
        if (state.tiles[i] == 0) {
            return i;
        }
    }
    return kPuzzleTileCount;
}

inline bool isTileCorrect(const PuzzleState& state, uint8_t position) {
    return position + 1 < kPuzzleTileCount &&
           state.tiles[position] == position + 1;
}

inline bool isPuzzleSolved(const PuzzleState& state) {
    for (uint8_t position = 0; position + 1 < kPuzzleTileCount; ++position) {
        if (!isTileCorrect(state, position)) {
            return false;
        }
    }
    return state.tiles[kPuzzleTileCount - 1] == 0;
}

inline bool isSamePuzzle(const PuzzleState& first, const PuzzleState& second) {
    if (first.gameId != second.gameId || first.revision != second.revision ||
        first.turnBoardId != second.turnBoardId) {
        return false;
    }
    for (uint8_t i = 0; i < kPuzzleTileCount; ++i) {
        if (first.tiles[i] != second.tiles[i]) {
            return false;
        }
    }
    return true;
}

enum class PuzzleVersionOrder : uint8_t { Older, Same, Newer };

inline PuzzleVersionOrder comparePuzzleVersion(const PuzzleState& current,
                                               const PuzzleState& candidate) {
    if (candidate.gameId != current.gameId) {
        return candidate.gameId > current.gameId ? PuzzleVersionOrder::Newer
                                                : PuzzleVersionOrder::Older;
    }
    if (candidate.revision != current.revision) {
        return candidate.revision > current.revision ? PuzzleVersionOrder::Newer
                                                    : PuzzleVersionOrder::Older;
    }
    return PuzzleVersionOrder::Same;
}

inline uint32_t puzzleStateDigest(const PuzzleState& state) {
    uint32_t digest = 2166136261u;
    const auto addByte = [&digest](uint8_t value) {
        digest ^= value;
        digest *= 16777619u;
    };
    for (uint8_t tile : state.tiles) {
        addByte(tile);
    }
    const uint32_t fields[] = {state.gameId, state.revision,
                               state.turnBoardId};
    for (uint32_t field : fields) {
        for (uint8_t shift = 0; shift < 32; shift += 8) {
            addByte(static_cast<uint8_t>(field >> shift));
        }
    }
    return digest;
}

inline bool isDeliverySuperseded(const PuzzleState& pending,
                                 const PuzzleState& observed) {
    return pending.gameId == observed.gameId &&
           pending.revision < observed.revision;
}

enum class ReconciliationAction : uint8_t {
    None,
    SendFullState,
    RequestFullState,
};

inline ReconciliationAction decideReconciliation(
    const PuzzleState& local, uint32_t remoteGameId, uint32_t remoteRevision,
    uint32_t remoteDigest, bool localIsAuthority) {
    if (remoteGameId < local.gameId ||
        (remoteGameId == local.gameId && remoteRevision < local.revision)) {
        return ReconciliationAction::SendFullState;
    }
    if (remoteGameId > local.gameId ||
        (remoteGameId == local.gameId && remoteRevision > local.revision)) {
        return ReconciliationAction::RequestFullState;
    }
    if (remoteDigest == puzzleStateDigest(local)) {
        return ReconciliationAction::None;
    }
    return localIsAuthority ? ReconciliationAction::SendFullState
                            : ReconciliationAction::RequestFullState;
}

inline bool shouldAdoptFullState(const PuzzleState& current,
                                 const PuzzleState& incoming) {
    return isValidPuzzle(incoming) &&
           comparePuzzleVersion(current, incoming) == PuzzleVersionOrder::Newer;
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
        incoming.gameId != current.gameId ||
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
