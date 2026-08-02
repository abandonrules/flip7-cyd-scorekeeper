/**
 * Serialization Tests - Round-trip encoding/decoding
 */

#include <gtest/gtest.h>
#include "flip7/protocol.hpp"
#include "flip7/serializer.hpp"

using namespace flip7;

TEST(SerializationTest, PacketHeaderRoundTrip) {
    PacketHeader header{};
    header.magic = kProtocolMagic;
    header.version = kProtocolVersion;
    header.type = MessageType::Heartbeat;
    header.senderId = 0x12345678;
    header.sessionId = 0x87654321;
    header.sequence = 42;

    uint8_t buffer[20];
    uint8_t* dst = buffer;
    serialize(header, dst);

    const uint8_t* src = buffer;
    PacketHeader decoded{};
    deserialize(src, decoded);

    EXPECT_EQ(decoded.magic, header.magic);
    EXPECT_EQ(decoded.version, header.version);
    EXPECT_EQ(decoded.type, header.type);
    EXPECT_EQ(decoded.senderId, header.senderId);
    EXPECT_EQ(decoded.sessionId, header.sessionId);
    EXPECT_EQ(decoded.sequence, header.sequence);
}

TEST(SerializationTest, HeartbeatPacketRoundTrip) {
    HeartbeatPacket packet{};
    packet.header.magic = kProtocolMagic;
    packet.header.version = kProtocolVersion;
    packet.header.type = MessageType::Heartbeat;
    packet.header.senderId = 0xD4E9F46A;
    packet.header.sessionId = 0xAABBCCDD;
    packet.header.sequence = 1;
    packet.uptimeMs = 123456;

    uint8_t buffer[24];
    uint8_t* dst = buffer;
    serialize(packet, dst);

    const uint8_t* src = buffer;
    HeartbeatPacket decoded{};
    deserialize(src, decoded);

    EXPECT_EQ(decoded.header.magic, packet.header.magic);
    EXPECT_EQ(decoded.header.type, packet.header.type);
    EXPECT_EQ(decoded.uptimeMs, packet.uptimeMs);
}

TEST(SerializationTest, PuzzleStatePacketRoundTrip) {
    PuzzleStatePacket packet{};
    packet.header.magic = kProtocolMagic;
    packet.header.version = kProtocolVersion;
    packet.header.type = MessageType::PuzzleState;
    packet.header.senderId = 0xD4E9F46A;
    packet.header.sessionId = 0xAABBCCDD;
    packet.header.sequence = 1;
    packet.state.columns = 3;
    packet.state.rows = 3;
    packet.state.theme = PuzzleTheme::Planets;
    packet.state.phase = PuzzlePhase::Playing;
    packet.state.gameId = 0x87654321;
    packet.state.revision = 10;
    packet.state.turnBoardId = 0xB0CBD8E7;
    for (int i = 0; i < 9; ++i) packet.state.tiles[i] = i + 1;
    packet.state.tiles[8] = 0;

    uint8_t buffer[68];
    uint8_t* dst = buffer;
    serialize(packet, dst);

    const uint8_t* src = buffer;
    PuzzleStatePacket decoded{};
    deserialize(src, decoded);

    EXPECT_EQ(decoded.header.magic, packet.header.magic);
    EXPECT_EQ(decoded.header.type, packet.header.type);
    EXPECT_EQ(decoded.state.columns, packet.state.columns);
    EXPECT_EQ(decoded.state.theme, packet.state.theme);
    EXPECT_EQ(decoded.state.gameId, packet.state.gameId);
    for (int i = 0; i < 30; ++i) {
        EXPECT_EQ(decoded.state.tiles[i], packet.state.tiles[i]);
    }
}

TEST(SerializationTest, PuzzleAckPacketRoundTrip) {
    PuzzleAckPacket packet{};
    packet.header.magic = kProtocolMagic;
    packet.header.version = kProtocolVersion;
    packet.header.type = MessageType::PuzzleAck;
    packet.header.senderId = 0xD4E9F46A;
    packet.header.sessionId = 0xAABBCCDD;
    packet.header.sequence = 5;
    packet.targetBoardId = 0xB0CBD8E7;
    packet.gameId = 0x87654321;
    packet.revision = 10;
    packet.stateDigest = 0xDEADBEEF;
    packet.acknowledgedType = MessageType::PuzzleState;
    packet.reserved[0] = 0;
    packet.reserved[1] = 0;
    packet.reserved[2] = 0;

    uint8_t buffer[40];
    uint8_t* dst = buffer;
    serialize(packet, dst);

    const uint8_t* src = buffer;
    PuzzleAckPacket decoded{};
    deserialize(src, decoded);

    EXPECT_EQ(decoded.header.magic, packet.header.magic);
    EXPECT_EQ(decoded.targetBoardId, packet.targetBoardId);
    EXPECT_EQ(decoded.gameId, packet.gameId);
    EXPECT_EQ(decoded.stateDigest, packet.stateDigest);
}

TEST(SerializationTest, StateRequestPacketRoundTrip) {
    StateRequestPacket packet{};
    packet.header.magic = kProtocolMagic;
    packet.header.version = kProtocolVersion;
    packet.header.type = MessageType::RequestState;
    packet.header.senderId = 0xD4E9F46A;
    packet.header.sessionId = 0xAABBCCDD;
    packet.header.sequence = 10;
    packet.gameId = 0x12345678;
    packet.revision = 20;
    packet.stateDigest = 0xCAFEBABE;

    uint8_t buffer[32];
    uint8_t* dst = buffer;
    serialize(packet, dst);

    const uint8_t* src = buffer;
    StateRequestPacket decoded{};
    deserialize(src, decoded);

    EXPECT_EQ(decoded.gameId, packet.gameId);
    EXPECT_EQ(decoded.revision, packet.revision);
    EXPECT_EQ(decoded.stateDigest, packet.stateDigest);
}

TEST(SerializationTest, GameStatePacketRoundTrip) {
    GameStatePacket packet{};
    packet.header.magic = kProtocolMagic;
    packet.header.version = kProtocolVersion;
    packet.header.type = MessageType::MastermindState;
    packet.header.senderId = 0xD4E9F46A;
    packet.header.sessionId = 0xAABBCCDD;
    packet.header.sequence = 100;
    packet.state.guessCount = 3;
    packet.state.codemakerBoardId = 0xD4E9F46A & 0xFF;
    packet.state.phase = MastermindPhase::Playing;
    packet.state.gameId = 0x11111111;
    packet.state.revision = 5;
    packet.state.hostBoardId = 0xD4E9F46A;
    packet.state.guestBoardId = 0xB0CBD8E7;
    packet.state.secret.colors[0] = 1;
    packet.state.secret.colors[1] = 2;
    packet.state.secret.colors[2] = 3;
    packet.state.secret.colors[3] = 4;

    uint8_t buffer[236];
    uint8_t* dst = buffer;
    serialize(packet, dst);

    const uint8_t* src = buffer;
    GameStatePacket decoded{};
    deserialize(src, decoded);

    EXPECT_EQ(decoded.header.magic, packet.header.magic);
    EXPECT_EQ(decoded.header.type, packet.header.type);
    EXPECT_EQ(decoded.state.guessCount, packet.state.guessCount);
    EXPECT_EQ(decoded.state.phase, packet.state.phase);
    EXPECT_EQ(decoded.state.gameId, packet.state.gameId);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(decoded.state.secret.colors[i], packet.state.secret.colors[i]);
    }
}

TEST(SerializationTest, PeekPacketType) {
    // Valid packet
    uint8_t buffer[68];
    uint8_t* dst = buffer;
    PuzzleStatePacket pkt{};
    pkt.header.magic = kProtocolMagic;
    pkt.header.version = kProtocolVersion;
    pkt.header.type = MessageType::PuzzleState;
    serialize(pkt, dst);

    MessageType type = peekPacketType(buffer, sizeof(buffer));
    EXPECT_EQ(type, MessageType::PuzzleState);

    // Wrong magic
    buffer[0] = 0xFF;
    MessageType bad = peekPacketType(buffer, sizeof(buffer));
    EXPECT_EQ(static_cast<uint8_t>(bad), 0);

    // Wrong version
    buffer[0] = static_cast<uint8_t>(kProtocolMagic & 0xFF);
    buffer[1] = static_cast<uint8_t>((kProtocolMagic >> 8) & 0xFF);
    buffer[2] = static_cast<uint8_t>((kProtocolMagic >> 16) & 0xFF);
    buffer[3] = static_cast<uint8_t>((kProtocolMagic >> 24) & 0xFF);
    buffer[4] = 99; // wrong version
    MessageType badVer = peekPacketType(buffer, sizeof(buffer));
    EXPECT_EQ(static_cast<uint8_t>(badVer), 0);

    // Too short
    MessageType shortLen = peekPacketType(buffer, 5);
    EXPECT_EQ(static_cast<uint8_t>(shortLen), 0);
}