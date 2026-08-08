/**
 * Protocol Tests - Core protocol definitions and utilities
 */

#include <gtest/gtest.h>
#include "flip7/protocol.hpp"

using namespace flip7;

TEST(ProtocolTest, ProtocolConstants) {
    EXPECT_EQ(kProtocolMagic, 0x46374359u);
    EXPECT_EQ(kProtocolVersion, 7);
    EXPECT_EQ(kPacketHeaderSize, 20);
    EXPECT_EQ(kHeartbeatPacketSize, 24);
    EXPECT_EQ(kPuzzleStatePacketSize, 68);
    EXPECT_EQ(kPuzzleAckPacketSize, 40);
    EXPECT_EQ(kStateRequestPacketSize, 32);
    EXPECT_EQ(kGameStatePacketSize, 236);
    EXPECT_EQ(kGameAckPacketSize, 40);
    EXPECT_EQ(kMastermindStateRequestPacketSize, 32);
    EXPECT_EQ(kPuzzleMaxTiles, 30);
    EXPECT_EQ(kMastermindColorCount, 7);
    EXPECT_EQ(kMastermindCodeLength, 4);
    EXPECT_EQ(kMastermindMaxGuesses, 10);
}

TEST(ProtocolTest, SequenceComparison) {
    EXPECT_TRUE(isSequenceNewer(5, 4));
    EXPECT_TRUE(isSequenceNewer(1, 0));
    EXPECT_TRUE(isSequenceNewer(0, UINT32_MAX));
    EXPECT_FALSE(isSequenceNewer(4, 5));
    EXPECT_FALSE(isSequenceNewer(0, 0));
}

TEST(ProtocolTest, SessionTracking) {
    uint32_t sessions[] = {100, 200, 300};
    EXPECT_TRUE(hasSeenSession(100, sessions, 3));
    EXPECT_TRUE(hasSeenSession(200, sessions, 3));
    EXPECT_FALSE(hasSeenSession(150, sessions, 3));
    EXPECT_FALSE(hasSeenSession(0, sessions, 3));
}

TEST(ProtocolTest, PeerSessionUsage) {
    uint32_t retired[] = {150, 180};
    EXPECT_TRUE(canUsePeerSession(100, 100, false, retired, 2));
    EXPECT_TRUE(canUsePeerSession(100, 200, true, retired, 2));
    EXPECT_FALSE(canUsePeerSession(100, 150, true, retired, 2));
    EXPECT_FALSE(canUsePeerSession(100, 200, false, retired, 2));
    EXPECT_FALSE(canUsePeerSession(100, 0, true, retired, 2));
}

TEST(ProtocolTest, Enums) {
    EXPECT_EQ(static_cast<uint8_t>(MessageType::Heartbeat), 1);
    EXPECT_EQ(static_cast<uint8_t>(MessageType::PuzzleState), 5);
    EXPECT_EQ(static_cast<uint8_t>(MessageType::PuzzleAck), 6);
    EXPECT_EQ(static_cast<uint8_t>(MessageType::MastermindState), 7);

    EXPECT_EQ(static_cast<uint8_t>(PuzzlePhase::Playing), 1);
    EXPECT_EQ(static_cast<uint8_t>(PuzzlePhase::Exited), 2);

    EXPECT_EQ(static_cast<uint8_t>(MastermindPhase::Setup), 1);
    EXPECT_EQ(static_cast<uint8_t>(MastermindPhase::Playing), 2);
    EXPECT_EQ(static_cast<uint8_t>(MastermindPhase::RoundComplete), 3);
    EXPECT_EQ(static_cast<uint8_t>(MastermindPhase::Exited), 4);
}

TEST(ProtocolTest, MastermindCode) {
    MastermindCode first{1, 2, 3, 4};
    MastermindCode second{1, 2, 3, 4};
    MastermindCode diff{1, 2, 3, 5};

    EXPECT_TRUE(sameMastermindCode(first, second));
    EXPECT_FALSE(sameMastermindCode(first, diff));
}
