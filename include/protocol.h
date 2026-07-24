#pragma once

#include <cstddef>
#include <cstdint>

#include "puzzle_logic.h"

constexpr uint32_t kProtocolMagic = 0x46374359;
constexpr uint8_t kProtocolVersion = 5;

inline bool isSequenceNewer(uint32_t candidate, uint32_t previous) {
    return static_cast<int32_t>(candidate - previous) > 0;
}

inline bool hasSeenSession(uint32_t candidate, const uint32_t* sessions,
                           size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (sessions[index] == candidate) {
            return true;
        }
    }
    return false;
}

inline bool canUsePeerSession(uint32_t current, uint32_t candidate,
                              bool peerOffline,
                              const uint32_t* retired, size_t retiredCount) {
    return candidate != 0 &&
           (current == 0 || candidate == current ||
            (peerOffline && !hasSeenSession(candidate, retired, retiredCount)));
}

enum class MessageType : uint8_t {
    Heartbeat = 1,
    ScoreUpdate,
    FullState,
    RequestState,
    PuzzleState,
    PuzzleAck,
};

struct PacketHeader {
    uint32_t magic;
    uint8_t version;
    MessageType type;
    uint32_t senderId;
    uint32_t sessionId;
    uint32_t sequence;
};

struct HeartbeatPacket {
    PacketHeader header;
    uint32_t uptimeMs;
};

static_assert(sizeof(PacketHeader) == 20, "PacketHeader wire format changed");
static_assert(sizeof(HeartbeatPacket) == 24,
              "HeartbeatPacket wire format changed");

struct PuzzleStatePacket {
    PacketHeader header;
    ::PuzzleState state;
};

static_assert(sizeof(PuzzleStatePacket) == 68,
              "PuzzleStatePacket wire format changed");

// The protocol is memcpy'd raw between two identically-compiled ESP32 targets.
// Pin the wire-visible field offsets so any accidental reordering or padding
// change fails at compile time rather than silently corrupting sync.
static_assert(offsetof(::PuzzleState, tiles) == 0, "tiles offset changed");
static_assert(offsetof(::PuzzleState, columns) == 30, "columns offset changed");
static_assert(offsetof(::PuzzleState, rows) == 31, "rows offset changed");
static_assert(offsetof(::PuzzleState, theme) == 32, "theme offset changed");
static_assert(offsetof(::PuzzleState, phase) == 33, "phase offset changed");
static_assert(offsetof(::PuzzleState, gameId) == 36, "gameId offset changed");
static_assert(offsetof(::PuzzleState, revision) == 40,
              "revision offset changed");
static_assert(offsetof(::PuzzleState, turnBoardId) == 44,
              "turnBoardId offset changed");
static_assert(sizeof(::PuzzleState) == 48, "PuzzleState size changed");
static_assert(offsetof(PacketHeader, senderId) == 8,
              "PacketHeader senderId offset changed");

struct PuzzleAckPacket {
    PacketHeader header;
    uint32_t targetBoardId;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
    MessageType acknowledgedType;
    uint8_t reserved[3];
};

static_assert(sizeof(PuzzleAckPacket) == 40,
              "PuzzleAckPacket wire format changed");

struct StateRequestPacket {
    PacketHeader header;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
};

static_assert(sizeof(StateRequestPacket) == 32,
              "StateRequestPacket wire format changed");
