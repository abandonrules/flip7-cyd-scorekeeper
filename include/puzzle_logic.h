#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint8_t kPuzzleMaxTiles = 30;

enum class PuzzleTheme : uint8_t {
    Greek = 1,
    Planets,
};

enum class PuzzlePhase : uint8_t {
    Playing = 1,
    Exited,
};

struct PuzzleSpec {
    uint8_t columns;
    uint8_t rows;
    PuzzleTheme theme;
};

struct PuzzleState {
    uint8_t tiles[kPuzzleMaxTiles];
    uint8_t columns;
    uint8_t rows;
    PuzzleTheme theme;
    PuzzlePhase phase;
    uint32_t gameId;
    uint32_t revision;
    uint32_t turnBoardId;
};

inline bool samePuzzleSpec(const PuzzleSpec& first, const PuzzleSpec& second) {
    return first.columns == second.columns && first.rows == second.rows &&
           first.theme == second.theme;
}

inline bool isSupportedPuzzleSpec(const PuzzleSpec& spec) {
    return (spec.columns == 3 && spec.rows == 3 &&
            spec.theme == PuzzleTheme::Planets) ||
           (spec.columns == 4 && spec.rows == 3 &&
            spec.theme == PuzzleTheme::Greek);
}

inline PuzzleSpec puzzleSpec(const PuzzleState& state) {
    return {state.columns, state.rows, state.theme};
}

inline uint8_t puzzleTileCount(const PuzzleState& state) {
    return static_cast<uint8_t>(state.columns * state.rows);
}

inline PuzzleState makeInitialPuzzle(uint32_t firstTurnBoardId,
                                     uint32_t gameId,
                                     const PuzzleSpec& spec) {
    PuzzleState state{};
    if (!isSupportedPuzzleSpec(spec)) {
        return state;
    }
    state.columns = spec.columns;
    state.rows = spec.rows;
    state.theme = spec.theme;
    state.phase = PuzzlePhase::Playing;
    const uint8_t count = puzzleTileCount(state);
    for (uint8_t i = 0; i + 1 < count; ++i) {
        state.tiles[i] = i + 1;
    }
    state.gameId = gameId;
    state.turnBoardId = firstTurnBoardId;
    return state;
}

inline bool arePuzzlePositionsAdjacent(const PuzzleState& state,
                                       uint8_t first, uint8_t second) {
    const uint8_t count = puzzleTileCount(state);
    if (first >= count || second >= count || state.columns == 0) {
        return false;
    }
    const uint8_t firstRow = first / state.columns;
    const uint8_t firstColumn = first % state.columns;
    const uint8_t secondRow = second / state.columns;
    const uint8_t secondColumn = second % state.columns;
    const uint8_t rowDistance = firstRow > secondRow ? firstRow - secondRow
                                                      : secondRow - firstRow;
    const uint8_t columnDistance = firstColumn > secondColumn
                                       ? firstColumn - secondColumn
                                       : secondColumn - firstColumn;
    return rowDistance + columnDistance == 1;
}

inline PuzzleState makeScrambledPuzzle(uint32_t firstTurnBoardId,
                                       uint32_t gameId,
                                       const PuzzleSpec& spec) {
    PuzzleState state = makeInitialPuzzle(firstTurnBoardId, gameId, spec);
    if (!isSupportedPuzzleSpec(spec)) {
        return state;
    }

    const uint8_t count = puzzleTileCount(state);
    uint8_t blank = count - 1;
    uint8_t previousBlank = kPuzzleMaxTiles;
    uint32_t random = gameId ^ firstTurnBoardId ^
                      (static_cast<uint32_t>(spec.columns) << 24) ^
                      (static_cast<uint32_t>(spec.rows) << 16) ^
                      (static_cast<uint32_t>(spec.theme) << 8);
    const uint16_t steps = static_cast<uint16_t>(count) * 8;
    for (uint16_t step = 0; step < steps; ++step) {
        uint8_t candidates[4]{};
        uint8_t candidateCount = 0;
        for (uint8_t position = 0; position < count; ++position) {
            if (position != previousBlank &&
                arePuzzlePositionsAdjacent(state, position, blank)) {
                candidates[candidateCount++] = position;
            }
        }
        random = random * 1664525u + 1013904223u;
        const uint8_t next = candidates[random % candidateCount];
        state.tiles[blank] = state.tiles[next];
        state.tiles[next] = 0;
        previousBlank = blank;
        blank = next;
    }
    bool solved = blank == count - 1;
    for (uint8_t position = 0; solved && position + 1 < count; ++position) {
        solved = state.tiles[position] == position + 1;
    }
    if (solved) {
        const uint8_t next = count - 2;
        state.tiles[blank] = state.tiles[next];
        state.tiles[next] = 0;
    }
    return state;
}

inline bool isSolvablePuzzleArrangement(const PuzzleState& state) {
    const uint8_t count = puzzleTileCount(state);
    uint16_t inversions = 0;
    for (uint8_t first = 0; first < count; ++first) {
        if (state.tiles[first] == 0) {
            continue;
        }
        for (uint8_t second = first + 1; second < count; ++second) {
            if (state.tiles[second] != 0 &&
                state.tiles[first] > state.tiles[second]) {
                ++inversions;
            }
        }
    }
    if (state.columns % 2 == 1) {
        return inversions % 2 == 0;
    }
    uint8_t blank = count;
    for (uint8_t position = 0; position < count; ++position) {
        if (state.tiles[position] == 0) {
            blank = position;
            break;
        }
    }
    if (blank == count) {
        return false;
    }
    const uint8_t blankRowFromBottom =
        static_cast<uint8_t>(state.rows - blank / state.columns);
    return (inversions + blankRowFromBottom) % 2 == 1;
}

inline bool isValidPuzzle(const PuzzleState& state) {
    const PuzzleSpec spec = puzzleSpec(state);
    if (!isSupportedPuzzleSpec(spec) ||
        (state.phase != PuzzlePhase::Playing &&
         state.phase != PuzzlePhase::Exited) ||
        state.gameId == 0 || state.turnBoardId == 0) {
        return false;
    }
    const uint8_t count = puzzleTileCount(state);
    bool seen[kPuzzleMaxTiles]{};
    for (uint8_t position = 0; position < count; ++position) {
        const uint8_t tile = state.tiles[position];
        if (tile >= count || seen[tile]) {
            return false;
        }
        seen[tile] = true;
    }
    for (uint8_t position = count; position < kPuzzleMaxTiles; ++position) {
        if (state.tiles[position] != 0) {
            return false;
        }
    }
    return isSolvablePuzzleArrangement(state);
}

inline uint8_t findBlank(const PuzzleState& state) {
    const uint8_t count = puzzleTileCount(state);
    for (uint8_t i = 0; i < count; ++i) {
        if (state.tiles[i] == 0) {
            return i;
        }
    }
    return count;
}

inline bool isTileCorrect(const PuzzleState& state, uint8_t position) {
    const uint8_t count = puzzleTileCount(state);
    return position + 1 < count && state.tiles[position] == position + 1;
}

inline bool isPuzzleSolved(const PuzzleState& state) {
    if (!isValidPuzzle(state) || state.phase != PuzzlePhase::Playing) {
        return false;
    }
    const uint8_t count = puzzleTileCount(state);
    for (uint8_t position = 0; position + 1 < count; ++position) {
        if (!isTileCorrect(state, position)) {
            return false;
        }
    }
    return state.tiles[count - 1] == 0;
}

inline bool isSamePuzzle(const PuzzleState& first, const PuzzleState& second) {
    if (!samePuzzleSpec(puzzleSpec(first), puzzleSpec(second)) ||
        first.phase != second.phase || first.gameId != second.gameId ||
        first.revision != second.revision ||
        first.turnBoardId != second.turnBoardId) {
        return false;
    }
    for (uint8_t i = 0; i < kPuzzleMaxTiles; ++i) {
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
    addByte(state.columns);
    addByte(state.rows);
    addByte(static_cast<uint8_t>(state.theme));
    addByte(static_cast<uint8_t>(state.phase));
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

inline bool tryPuzzleMove(PuzzleState& state, uint8_t position,
                          uint32_t actorBoardId, uint32_t nextTurnBoardId) {
    if (!isValidPuzzle(state) || state.phase != PuzzlePhase::Playing ||
        state.turnBoardId != actorBoardId ||
        actorBoardId == nextTurnBoardId || state.revision == UINT32_MAX ||
        position >= puzzleTileCount(state) || state.tiles[position] == 0) {
        return false;
    }
    const uint8_t blank = findBlank(state);
    if (!arePuzzlePositionsAdjacent(state, position, blank)) {
        return false;
    }
    state.tiles[blank] = state.tiles[position];
    state.tiles[position] = 0;
    ++state.revision;
    state.turnBoardId = nextTurnBoardId;
    return true;
}

inline bool exitPuzzle(PuzzleState& state, uint32_t actorBoardId,
                       uint32_t otherBoardId) {
    if (!isValidPuzzle(state) || state.phase != PuzzlePhase::Playing ||
        actorBoardId == 0 || otherBoardId == 0 ||
        actorBoardId == otherBoardId || state.revision == UINT32_MAX ||
        (state.turnBoardId != actorBoardId &&
         state.turnBoardId != otherBoardId)) {
        return false;
    }
    state.phase = PuzzlePhase::Exited;
    ++state.revision;
    return true;
}

inline bool applyPuzzleExitSignal(PuzzleState& current,
                                  const PuzzleState& candidate,
                                  uint32_t senderBoardId,
                                  uint32_t recipientBoardId) {
    if (!isValidPuzzle(current) || !isValidPuzzle(candidate) ||
        candidate.phase != PuzzlePhase::Exited ||
        current.gameId != candidate.gameId ||
        !samePuzzleSpec(puzzleSpec(current), puzzleSpec(candidate)) ||
        senderBoardId == 0 || recipientBoardId == 0 ||
        senderBoardId == recipientBoardId ||
        (current.turnBoardId != senderBoardId &&
         current.turnBoardId != recipientBoardId)) {
        return false;
    }
    if (candidate.turnBoardId != senderBoardId &&
        candidate.turnBoardId != recipientBoardId) {
        return false;
    }
    const PuzzleVersionOrder order = comparePuzzleVersion(current, candidate);
    const bool equalConflict =
        order == PuzzleVersionOrder::Same && !isSamePuzzle(current, candidate);
    if (order == PuzzleVersionOrder::Newer ||
        (equalConflict && senderBoardId < recipientBoardId)) {
        current = candidate;
        return true;
    }
    if (current.phase == PuzzlePhase::Playing) {
        return exitPuzzle(current, senderBoardId, recipientBoardId);
    }
    return true;
}

inline bool isValidRemoteTransition(const PuzzleState& current,
                                    const PuzzleState& incoming,
                                    uint32_t senderBoardId,
                                    uint32_t recipientBoardId) {
    if (!isValidPuzzle(current) || !isValidPuzzle(incoming) ||
        current.phase != PuzzlePhase::Playing ||
        incoming.phase != PuzzlePhase::Playing ||
        !samePuzzleSpec(puzzleSpec(current), puzzleSpec(incoming)) ||
        incoming.gameId != current.gameId ||
        current.turnBoardId != senderBoardId ||
        incoming.turnBoardId != recipientBoardId ||
        incoming.revision != current.revision + 1) {
        return false;
    }

    const uint8_t oldBlank = findBlank(current);
    const uint8_t newBlank = findBlank(incoming);
    if (!arePuzzlePositionsAdjacent(current, oldBlank, newBlank) ||
        incoming.tiles[oldBlank] != current.tiles[newBlank]) {
        return false;
    }

    for (uint8_t i = 0; i < kPuzzleMaxTiles; ++i) {
        if (i != oldBlank && i != newBlank &&
            incoming.tiles[i] != current.tiles[i]) {
            return false;
        }
    }
    return true;
}

inline bool isValidPuzzleForParticipants(const PuzzleState& state,
                                         uint32_t firstBoardId,
                                         uint32_t secondBoardId) {
    if (!isValidPuzzle(state) || firstBoardId == 0 || secondBoardId == 0 ||
        firstBoardId == secondBoardId) {
        return false;
    }
    if (state.turnBoardId != firstBoardId && state.turnBoardId != secondBoardId) {
        return false;
    }
    // EXIT is legal from either player regardless of whose move it is, so an
    // Exited state deliberately breaks move-parity; its sender/turn validity is
    // gated by applyPuzzleExitSignal. Only enforce parity while Playing.
    if (state.phase == PuzzlePhase::Exited) {
        return state.revision != 0;
    }
    const uint32_t host = firstBoardId < secondBoardId ? firstBoardId
                                                       : secondBoardId;
    const uint32_t guest = firstBoardId < secondBoardId ? secondBoardId
                                                        : firstBoardId;
    const uint32_t expectedTurn = state.revision % 2 == 0 ? host : guest;
    return state.turnBoardId == expectedTurn;
}

inline bool shouldAdoptFullState(const PuzzleState& current,
                                 const PuzzleState& incoming,
                                 uint32_t senderBoardId,
                                 uint32_t recipientBoardId) {
    if (!isValidPuzzleForParticipants(incoming, senderBoardId,
                                      recipientBoardId) ||
        incoming.phase != PuzzlePhase::Playing ||
        comparePuzzleVersion(current, incoming) != PuzzleVersionOrder::Newer) {
        return false;
    }
    const uint32_t host = senderBoardId < recipientBoardId ? senderBoardId
                                                           : recipientBoardId;
    if (incoming.gameId != current.gameId) {
        return incoming.gameId > current.gameId && senderBoardId == host;
    }
    if (current.phase != PuzzlePhase::Playing ||
        !samePuzzleSpec(puzzleSpec(current), puzzleSpec(incoming))) {
        return false;
    }
    if (incoming.revision == current.revision + 1) {
        return isValidRemoteTransition(current, incoming, senderBoardId,
                                       recipientBoardId);
    }
    return senderBoardId == host;
}
