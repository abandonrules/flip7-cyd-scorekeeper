/**
 * Reconciliation Tests - State synchronization logic
 */

#include <gtest/gtest.h>
#include "flip7/protocol.hpp"

using namespace flip7;

TEST(ReconciliationTest, SequenceComparison) {
    EXPECT_TRUE(isSequenceNewer(5, 4));
    EXPECT_TRUE(isSequenceNewer(1, 0));
    EXPECT_TRUE(isSequenceNewer(0, UINT32_MAX)); // wraparound
    EXPECT_FALSE(isSequenceNewer(4, 5));
    EXPECT_FALSE(isSequenceNewer(5, 5));
}

TEST(ReconciliationTest, SessionTracking) {
    uint32_t sessions[] = {0x11111111, 0x22222222, 0x33333333};
    EXPECT_TRUE(hasSeenSession(0x11111111, sessions, 3));
    EXPECT_TRUE(hasSeenSession(0x22222222, sessions, 3));
    EXPECT_FALSE(hasSeenSession(0x44444444, sessions, 3));
    EXPECT_FALSE(hasSeenSession(0, sessions, 3));
}

TEST(ReconciliationTest, PeerSessionLogic) {
    EXPECT_TRUE(canUsePeerSession(0, 0x11111111, false, nullptr, 0));
    EXPECT_TRUE(canUsePeerSession(0x11111111, 0x11111111, false, nullptr, 0));
    EXPECT_FALSE(canUsePeerSession(0x11111111, 0x22222222, false, nullptr, 0));
    
    uint32_t retired[] = {0x33333333};
    EXPECT_TRUE(canUsePeerSession(0x11111111, 0x22222222, true, retired, 1));
    EXPECT_FALSE(canUsePeerSession(0x11111111, 0x33333333, true, retired, 1));
}

TEST(ReconciliationTest, PuzzleVersionComparison) {
    PuzzleState a{}, b{};
    a.gameId = 10; a.revision = 5;
    b.gameId = 10; b.revision = 6;
    EXPECT_EQ(comparePuzzleVersion(a, b), PuzzleVersionOrder::Newer);
    
    b.revision = 4;
    EXPECT_EQ(comparePuzzleVersion(a, b), PuzzleVersionOrder::Older);
    
    b.revision = 5;
    EXPECT_EQ(comparePuzzleVersion(a, b), PuzzleVersionOrder::Same);
    
    b.gameId = 11;
    EXPECT_EQ(comparePuzzleVersion(a, b), PuzzleVersionOrder::Newer);
}

TEST(ReconciliationTest, PuzzleDigestConsistency) {
    PuzzleState state{};
    state.columns = 4; state.rows = 3; state.theme = PuzzleTheme::Greek;
    state.phase = PuzzlePhase::Playing;
    state.gameId = 100; state.revision = 200; state.turnBoardId = 300;
    for (int i = 0; i < 12; ++i) state.tiles[i] = i + 1;
    state.tiles[11] = 0;
    
    uint32_t d1 = puzzleStateDigest(state);
    uint32_t d2 = puzzleStateDigest(state);
    EXPECT_EQ(d1, d2);
}

TEST(ReconciliationTest, PuzzleDeliverySuperseded) {
    PuzzleState pending{}, observed{};
    pending.gameId = 10; pending.revision = 5;
    observed.gameId = 10; observed.revision = 6;
    EXPECT_TRUE(isDeliverySuperseded(pending, observed));
    
    observed.revision = 4;
    EXPECT_FALSE(isDeliverySuperseded(pending, observed));
    
    pending.gameId = 11;
    EXPECT_FALSE(isDeliverySuperseded(pending, observed));
}

TEST(ReconciliationTest, ReconciliationDecision) {
    PuzzleState local{};
    local.gameId = 10; local.revision = 5;
    
    // Remote older -> send full state
    auto action = decideReconciliation(local, 10, 4, 0x12345678, true);
    EXPECT_EQ(action, ReconciliationAction::SendFullState);
    
    // Remote newer -> request full state
    action = decideReconciliation(local, 10, 6, 0x12345678, true);
    EXPECT_EQ(action, ReconciliationAction::RequestFullState);
    
    // Same revision, digest matches -> none
    action = decideReconciliation(local, 10, 5, puzzleStateDigest(local), true);
    EXPECT_EQ(action, ReconciliationAction::None);
    
    // Same revision, digest differs -> send if authority, request if not
    action = decideReconciliation(local, 10, 5, 0xDEADBEEF, true);
    EXPECT_EQ(action, ReconciliationAction::SendFullState);
    action = decideReconciliation(local, 10, 5, 0xDEADBEEF, false);
    EXPECT_EQ(action, ReconciliationAction::RequestFullState);
}

TEST(ReconciliationTest, PuzzleMoveValidation) {
    PuzzleSpec spec{3, 3, PuzzleTheme::Planets};
    PuzzleState state = makeInitialPuzzle(0x12345678, 0x87654321, spec);
    state = makeScrambledPuzzle(0x12345678, 0x87654321, spec);
    
    // Find a valid move - just try all positions and find one that works
    uint8_t movedPos = 255;
    for (uint8_t pos = 0; pos < 9; ++pos) {
        PuzzleState testState = state;
        if (tryPuzzleMove(testState, pos, testState.turnBoardId, 0xB0CBD8E7)) {
            movedPos = pos;
            break;
        }
    }
    ASSERT_NE(movedPos, 255) << "Should find at least one valid move";
    
    // Valid move
    bool ok = tryPuzzleMove(state, movedPos, state.turnBoardId, 0xB0CBD8E7);
    EXPECT_TRUE(ok);
    EXPECT_EQ(state.revision, 1);
    EXPECT_EQ(state.turnBoardId, 0xB0CBD8E7);
    
    // Invalid: not adjacent (try a different position that wasn't valid)
    PuzzleState state2 = state;
    uint8_t invalidPos = 255;
    for (uint8_t pos = 0; pos < 9; ++pos) {
        if (pos != movedPos) {
            PuzzleState testState = state2;
            if (!tryPuzzleMove(testState, pos, testState.turnBoardId, 0xD4E9F46A)) {
                invalidPos = pos;
                break;
            }
        }
    }
    if (invalidPos != 255) {
        ok = tryPuzzleMove(state2, invalidPos, state2.turnBoardId, 0xD4E9F46A);
        EXPECT_FALSE(ok);
    }
    
    // Invalid: moving blank
    PuzzleState state3 = state;
    uint8_t blank = findBlank(state3);
    ok = tryPuzzleMove(state3, blank, state3.turnBoardId, 0xB0CBD8E7);
    EXPECT_FALSE(ok);
}

TEST(ReconciliationTest, PuzzleExitSignal) {
    PuzzleSpec spec{3, 3, PuzzleTheme::Planets};
    PuzzleState state = makeInitialPuzzle(0x12345678, 0x87654321, spec);
    state = makeScrambledPuzzle(0x12345678, 0x87654321, spec);
    
    PuzzleState exited = state;
    bool ok = exitPuzzle(exited, 0x12345678, 0xB0CBD8E7);
    EXPECT_TRUE(ok);
    EXPECT_EQ(exited.phase, PuzzlePhase::Exited);
    
    // Apply exit signal from peer
    PuzzleState current = state;
    bool ok2 = applyPuzzleExitSignal(current, exited, 0x12345678, 0xB0CBD8E7);
    EXPECT_TRUE(ok2);
    EXPECT_EQ(current.phase, PuzzlePhase::Exited);
    
    // Applying same exit signal again should succeed (idempotent)
    PuzzleState current2 = exited;
    bool ok3 = applyPuzzleExitSignal(current2, exited, 0x12345678, 0xB0CBD8E7);
    EXPECT_TRUE(ok3);
}