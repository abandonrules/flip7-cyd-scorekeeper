#pragma once

#include <cstddef>
#include <cstdint>

#include "mastermind_logic.h"
#include "puzzle_logic.h"

constexpr uint32_t kProtocolMagic = 0x46374359;
constexpr uint8_t kProtocolVersion = 7;

// Returns true when candidate is strictly newer than previous using
// wrapping signed arithmetic (handles sequence-number rollover).
inline bool isSequenceNewer(uint32_t candidate, uint32_t previous) {
    return static_cast<int32_t>(candidate - previous) > 0;
}

// Returns true when candidate matches any entry in the sessions array.
inline bool hasSeenSession(uint32_t candidate, const uint32_t* sessions,
                           size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (sessions[index] == candidate) {
            return true;
        }
    }
    return false;
}

// Returns true when the candidate session ID is acceptable: first contact,
// known session, or unknown session from an offline peer.
inline bool canUsePeerSession(uint32_t current, uint32_t candidate,
                              bool peerOffline,
                              const uint32_t* retired, size_t retiredCount) {
    return candidate != 0 &&
           (current == 0 || candidate == current ||
            (peerOffline && !hasSeenSession(candidate, retired, retiredCount)));
}

// All ESP-NOW packet types exchanged between CYD boards.
enum class MessageType : uint8_t {
    Heartbeat = 1,
    ScoreUpdate,
    FullState,
    RequestState,
    PuzzleState,
    PuzzleAck,
    MastermindState,
    MastermindFullState,
    MastermindAck,
    MastermindRequestState,
};

// Fixed-size header prepended to every protocol packet.
// Fixed-size header prepended to every protocol packet.
struct PacketHeader {
    uint32_t magic;
    uint8_t version;
    MessageType type;
    uint32_t senderId;
    uint32_t sessionId;
    uint32_t sequence;
};

// Keepalive packet; carries uptime for diagnostics.
struct HeartbeatPacket {
    PacketHeader header;
    uint32_t uptimeMs;
};

// Carries a full PuzzleState snapshot for initial sync or reconciliation.
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

// Acknowledgement for a PuzzleState or FullState delivery.
struct PuzzleAckPacket {
    PacketHeader header;
    uint32_t targetBoardId;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
    MessageType acknowledgedType;
    uint8_t reserved[3];
};

// Requests the peer to send a full state snapshot on divergence.
struct StateRequestPacket {
    PacketHeader header;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
};

// Carries a full MastermindState snapshot.
struct GameStatePacket {
    PacketHeader header;
    MastermindState state;
};

// Acknowledgement for a MastermindState or MastermindFullState delivery.
struct GameAckPacket {
    PacketHeader header;
    uint32_t targetBoardId;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
    MessageType acknowledgedType;
    uint8_t reserved[3];
};

// Requests the peer to resend its authoritative MastermindState.
struct MastermindStateRequestPacket {
    PacketHeader header;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
};

static_assert(sizeof(PacketHeader) == 20, "PacketHeader wire format changed");
static_assert(sizeof(HeartbeatPacket) == 24,
              "HeartbeatPacket wire format changed");
static_assert(sizeof(PuzzleStatePacket) == 68,
              "PuzzleStatePacket wire format changed");
static_assert(sizeof(PuzzleAckPacket) == 40,
              "PuzzleAckPacket wire format changed");
static_assert(sizeof(StateRequestPacket) == 32,
              "StateRequestPacket wire format changed");
static_assert(sizeof(MastermindState) == 96,
              "MastermindState wire format changed");
static_assert(sizeof(GameStatePacket) == 116,
              "GameStatePacket wire format changed");
static_assert(sizeof(GameAckPacket) == 40,
              "GameAckPacket wire format changed");
static_assert(sizeof(MastermindStateRequestPacket) == 32,
              "MastermindStateRequestPacket wire format changed");