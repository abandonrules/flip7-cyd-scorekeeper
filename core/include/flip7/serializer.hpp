/**
 * Flip7 Protocol Packet Serialization
 * 
 * Explicit serialization/deserialization for protocol packets.
 * No memcpy of native structs - wire format is explicitly defined.
 */

#pragma once

#include "protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace flip7 {

// Endian: Little-endian on wire (matches ESP32 and most Android devices)
inline void writeU8(uint8_t*& dst, uint8_t value) {
    *dst++ = value;
}

inline void writeU16LE(uint8_t*& dst, uint16_t value) {
    *dst++ = static_cast<uint8_t>(value & 0xFF);
    *dst++ = static_cast<uint8_t>((value >> 8) & 0xFF);
}

inline void writeU32LE(uint8_t*& dst, uint32_t value) {
    *dst++ = static_cast<uint8_t>(value & 0xFF);
    *dst++ = static_cast<uint8_t>((value >> 8) & 0xFF);
    *dst++ = static_cast<uint8_t>((value >> 16) & 0xFF);
    *dst++ = static_cast<uint8_t>((value >> 24) & 0xFF);
}

inline uint8_t readU8(const uint8_t*& src) {
    return *src++;
}

inline uint16_t readU16LE(const uint8_t*& src) {
    uint16_t value = static_cast<uint16_t>(*src++);
    value |= static_cast<uint16_t>(*src++) << 8;
    return value;
}

inline uint32_t readU32LE(const uint8_t*& src) {
    uint32_t value = static_cast<uint32_t>(*src++);
    value |= static_cast<uint32_t>(*src++) << 8;
    value |= static_cast<uint32_t>(*src++) << 16;
    value |= static_cast<uint32_t>(*src++) << 24;
    return value;
}

// PacketHeader: 20 bytes
inline void serialize(const PacketHeader& src, uint8_t*& dst) {
    writeU32LE(dst, src.magic);
    writeU8(dst, src.version);
    writeU8(dst, static_cast<uint8_t>(src.type));
    writeU32LE(dst, src.senderId);
    writeU32LE(dst, src.sessionId);
    writeU32LE(dst, src.sequence);
}

inline void deserialize(const uint8_t*& src, PacketHeader& dst) {
    dst.magic = readU32LE(src);
    dst.version = readU8(src);
    dst.type = static_cast<MessageType>(readU8(src));
    dst.senderId = readU32LE(src);
    dst.sessionId = readU32LE(src);
    dst.sequence = readU32LE(src);
}

// HeartbeatPacket: 24 bytes
inline void serialize(const HeartbeatPacket& src, uint8_t*& dst) {
    serialize(src.header, dst);
    writeU32LE(dst, src.uptimeMs);
}

inline void deserialize(const uint8_t*& src, HeartbeatPacket& dst) {
    deserialize(src, dst.header);
    dst.uptimeMs = readU32LE(src);
}

// PuzzleStatePacket: 68 bytes (20 + 48)
inline void serialize(const PuzzleStatePacket& src, uint8_t*& dst) {
    serialize(src.header, dst);
    // Serialize PuzzleState (48 bytes)
    for (size_t i = 0; i < kPuzzleMaxTiles; ++i) {
        writeU8(dst, src.state.tiles[i]);
    }
    writeU8(dst, src.state.columns);
    writeU8(dst, src.state.rows);
    writeU8(dst, static_cast<uint8_t>(src.state.theme));
    writeU8(dst, static_cast<uint8_t>(src.state.phase));
    // 3 bytes padding to align gameId
    writeU8(dst, 0);
    writeU8(dst, 0);
    writeU8(dst, 0);
    writeU32LE(dst, src.state.gameId);
    writeU32LE(dst, src.state.revision);
    writeU32LE(dst, src.state.turnBoardId);
}

inline void deserialize(const uint8_t*& src, PuzzleStatePacket& dst) {
    deserialize(src, dst.header);
    // Deserialize PuzzleState (48 bytes)
    for (size_t i = 0; i < kPuzzleMaxTiles; ++i) {
        dst.state.tiles[i] = readU8(src);
    }
    dst.state.columns = readU8(src);
    dst.state.rows = readU8(src);
    dst.state.theme = static_cast<PuzzleTheme>(readU8(src));
    dst.state.phase = static_cast<PuzzlePhase>(readU8(src));
    // Skip 3 bytes padding
    src += 3;
    dst.state.gameId = readU32LE(src);
    dst.state.revision = readU32LE(src);
    dst.state.turnBoardId = readU32LE(src);
}

// PuzzleAckPacket: 40 bytes
inline void serialize(const PuzzleAckPacket& src, uint8_t*& dst) {
    serialize(src.header, dst);
    writeU32LE(dst, src.targetBoardId);
    writeU32LE(dst, src.gameId);
    writeU32LE(dst, src.revision);
    writeU32LE(dst, src.stateDigest);
    writeU8(dst, static_cast<uint8_t>(src.acknowledgedType));
    writeU8(dst, src.reserved[0]);
    writeU8(dst, src.reserved[1]);
    writeU8(dst, src.reserved[2]);
}

inline void deserialize(const uint8_t*& src, PuzzleAckPacket& dst) {
    deserialize(src, dst.header);
    dst.targetBoardId = readU32LE(src);
    dst.gameId = readU32LE(src);
    dst.revision = readU32LE(src);
    dst.stateDigest = readU32LE(src);
    dst.acknowledgedType = static_cast<MessageType>(readU8(src));
    dst.reserved[0] = readU8(src);
    dst.reserved[1] = readU8(src);
    dst.reserved[2] = readU8(src);
}

// StateRequestPacket: 32 bytes
inline void serialize(const StateRequestPacket& src, uint8_t*& dst) {
    serialize(src.header, dst);
    writeU32LE(dst, src.gameId);
    writeU32LE(dst, src.revision);
    writeU32LE(dst, src.stateDigest);
}

inline void deserialize(const uint8_t*& src, StateRequestPacket& dst) {
    deserialize(src, dst.header);
    dst.gameId = readU32LE(src);
    dst.revision = readU32LE(src);
    dst.stateDigest = readU32LE(src);
}

// GameStatePacket: 116 bytes (20 + 96)
inline void serialize(const GameStatePacket& src, uint8_t*& dst) {
    serialize(src.header, dst);
    // Serialize MastermindState (96 bytes)
    // MastermindGuess guesses[kMastermindMaxGuesses];
    for (size_t g = 0; g < kMastermindMaxGuesses; ++g) {
        for (size_t i = 0; i < kMastermindCodeLength; ++i) {
            writeU8(dst, src.state.guesses[g].code.colors[i]);
        }
        writeU8(dst, src.state.guesses[g].feedback.exact);
        writeU8(dst, src.state.guesses[g].feedback.colorOnly);
    }
    // MastermindCode secret;
    for (size_t i = 0; i < kMastermindCodeLength; ++i) {
        writeU8(dst, src.state.secret.colors[i]);
    }
    writeU32LE(dst, src.state.gameId);
    writeU32LE(dst, src.state.revision);
    writeU32LE(dst, src.state.hostBoardId);
    writeU32LE(dst, src.state.guestBoardId);
    writeU32LE(dst, src.state.codemakerBoardId);
    writeU32LE(dst, src.state.roundWinnerBoardId);
    writeU8(dst, src.state.hostScore);
    writeU8(dst, src.state.guestScore);
    writeU8(dst, src.state.round);
    writeU8(dst, src.state.guessCount);
    writeU8(dst, static_cast<uint8_t>(src.state.phase));
    writeU8(dst, src.state.reserved[0]);
    writeU8(dst, src.state.reserved[1]);
    writeU8(dst, src.state.reserved[2]);
}

inline void deserialize(const uint8_t*& src, GameStatePacket& dst) {
    deserialize(src, dst.header);
    // Deserialize MastermindState (96 bytes)
    // MastermindGuess guesses[kMastermindMaxGuesses];
    for (size_t g = 0; g < kMastermindMaxGuesses; ++g) {
        for (size_t i = 0; i < kMastermindCodeLength; ++i) {
            dst.state.guesses[g].code.colors[i] = readU8(src);
        }
        dst.state.guesses[g].feedback.exact = readU8(src);
        dst.state.guesses[g].feedback.colorOnly = readU8(src);
    }
    // MastermindCode secret;
    for (size_t i = 0; i < kMastermindCodeLength; ++i) {
        dst.state.secret.colors[i] = readU8(src);
    }
    dst.state.gameId = readU32LE(src);
    dst.state.revision = readU32LE(src);
    dst.state.hostBoardId = readU32LE(src);
    dst.state.guestBoardId = readU32LE(src);
    dst.state.codemakerBoardId = readU32LE(src);
    dst.state.roundWinnerBoardId = readU32LE(src);
    dst.state.hostScore = readU8(src);
    dst.state.guestScore = readU8(src);
    dst.state.round = readU8(src);
    dst.state.guessCount = readU8(src);
    dst.state.phase = static_cast<MastermindPhase>(readU8(src));
    dst.state.reserved[0] = readU8(src);
    dst.state.reserved[1] = readU8(src);
    dst.state.reserved[2] = readU8(src);
}

// GameAckPacket: 40 bytes
inline void serialize(const GameAckPacket& src, uint8_t*& dst) {
    serialize(src.header, dst);
    writeU32LE(dst, src.targetBoardId);
    writeU32LE(dst, src.gameId);
    writeU32LE(dst, src.revision);
    writeU32LE(dst, src.stateDigest);
    writeU8(dst, static_cast<uint8_t>(src.acknowledgedType));
    writeU8(dst, src.reserved[0]);
    writeU8(dst, src.reserved[1]);
    writeU8(dst, src.reserved[2]);
}

inline void deserialize(const uint8_t*& src, GameAckPacket& dst) {
    deserialize(src, dst.header);
    dst.targetBoardId = readU32LE(src);
    dst.gameId = readU32LE(src);
    dst.revision = readU32LE(src);
    dst.stateDigest = readU32LE(src);
    dst.acknowledgedType = static_cast<MessageType>(readU8(src));
    dst.reserved[0] = readU8(src);
    dst.reserved[1] = readU8(src);
    dst.reserved[2] = readU8(src);
}

// MastermindStateRequestPacket: 32 bytes
inline void serialize(const MastermindStateRequestPacket& src, uint8_t*& dst) {
    serialize(src.header, dst);
    writeU32LE(dst, src.gameId);
    writeU32LE(dst, src.revision);
    writeU32LE(dst, src.stateDigest);
}

inline void deserialize(const uint8_t*& src, MastermindStateRequestPacket& dst) {
    deserialize(src, dst.header);
    dst.gameId = readU32LE(src);
    dst.revision = readU32LE(src);
    dst.stateDigest = readU32LE(src);
}

// Generic packet detection
inline MessageType peekPacketType(const uint8_t* data, size_t length) {
    if (length < 6) return MessageType(0);
    const uint8_t* ptr = data;
    if (readU32LE(ptr) != kProtocolMagic) {
        return MessageType(0);
    }
    // readU32LE already advanced ptr by 4, so ptr is now at version byte
    uint8_t version = readU8(ptr);
    if (version != kProtocolVersion) {
        return MessageType(0);
    }
    return static_cast<MessageType>(readU8(ptr));
}

} // namespace flip7