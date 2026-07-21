#pragma once

#include <cstdint>

constexpr uint32_t kProtocolMagic = 0x46374359;
constexpr uint8_t kProtocolVersion = 1;

enum class MessageType : uint8_t {
    Heartbeat = 1,
    ScoreUpdate,
    FullState,
    RequestState,
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
