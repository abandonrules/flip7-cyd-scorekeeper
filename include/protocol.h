#pragma once

#include <cstdint>

#include "puzzle_logic.h"

constexpr uint32_t kProtocolMagic = 0x46374359;
constexpr uint8_t kProtocolVersion = 1;

enum class MessageType : uint8_t {
    Heartbeat = 1,
    ScoreUpdate,
    FullState,
    RequestState,
    PuzzleState,
};

struct HeartbeatPacket {
    uint32_t magic;
    uint8_t version;
    MessageType type;
    uint16_t reserved;
    uint32_t senderId;
    uint32_t sequence;
    uint32_t uptimeMs;
};

static_assert(sizeof(HeartbeatPacket) == 20,
              "HeartbeatPacket wire format changed");

struct PuzzleStatePacket {
    uint32_t magic;
    uint8_t version;
    MessageType type;
    uint16_t reserved;
    uint32_t senderId;
    uint32_t sequence;
    ::PuzzleState state;
};

static_assert(sizeof(PuzzleStatePacket) == 36,
              "PuzzleStatePacket wire format changed");
