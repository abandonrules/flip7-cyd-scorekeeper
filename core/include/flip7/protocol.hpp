/**
 * Flip7 Core Protocol Definitions
 * 
 * Platform-neutral protocol definitions shared between ESP32 and Android.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace flip7 {

// Protocol constants
constexpr uint32_t kProtocolMagic = 0x46374359; // "F7CY" in ASCII
constexpr uint8_t kProtocolVersion = 7;

// Packet structure sizes (wire format)
constexpr size_t kPacketHeaderSize = 20;
constexpr size_t kHeartbeatPacketSize = 24;
constexpr size_t kPuzzleStatePacketSize = 68;
constexpr size_t kPuzzleAckPacketSize = 40;
constexpr size_t kStateRequestPacketSize = 32;
constexpr size_t kGameStatePacketSize = 236;
constexpr size_t kGameAckPacketSize = 40;
constexpr size_t kMastermindStateRequestPacketSize = 32;

// Game limits
constexpr uint8_t kPuzzleMaxTiles = 30;
constexpr uint8_t kMastermindColorCount = 7;
constexpr uint8_t kMastermindCodeLength = 4;
constexpr uint8_t kMastermindMaxGuesses = 10;
constexpr uint8_t kAquariumMaxFish = 16;

// Mastermind types
struct MastermindCode {
    uint8_t colors[kMastermindCodeLength];
};

struct MastermindFeedback {
    uint8_t exact;
    uint8_t colorOnly;
};

struct MastermindGuess {
    MastermindCode code;
    MastermindFeedback feedback;
};

// Sequence comparison (handles wraparound)
inline bool isSequenceNewer(uint32_t candidate, uint32_t previous) {
    return static_cast<int32_t>(candidate - previous) > 0;
}

// Session tracking
inline bool hasSeenSession(uint32_t candidate, const uint32_t* sessions, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (sessions[i] == candidate) return true;
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

// Message types (must match wire protocol)
enum class MessageType : uint8_t {
    Heartbeat = 1,
    ScoreUpdate = 2,
    FullState = 3,
    RequestState = 4,
    PuzzleState = 5,
    PuzzleAck = 6,
    MastermindState = 7,
    MastermindFullState = 8,
    MastermindAck = 9,
    MastermindRequestState = 10,
    // BLE-specific extensions (100+)
    BleCommand = 100,
    BleEvent = 101,
    BleDeviceInfo = 102,
};

enum class PuzzleTheme : uint8_t {
    Greek = 1,
    Planets = 2,
};

enum class PuzzlePhase : uint8_t {
    Playing = 1,
    Exited = 2,
};

enum class MastermindPhase : uint8_t {
    Setup = 1,
    Playing = 2,
    RoundComplete = 3,
    Exited = 4,
};

enum class ActiveGameKind : uint8_t {
    Home = 0,
    Puzzle = 1,
    Mastermind = 2,
    Countdown = 3,
};

enum class PacketOrigin : uint8_t {
    Unknown = 0,
    CydHost = 1,
    CydGuest = 2,
    Android = 3,
};

enum class ClientRole : uint8_t {
    Spectator = 1,
    Player = 2,
    Controller = 3,
    Administrator = 4,
};

// Puzzle specification
struct PuzzleSpec {
    uint8_t columns;
    uint8_t rows;
    PuzzleTheme theme;
};

// Packet origin field added to header for BLE routing
struct ExtendedPacketHeader {
    uint32_t magic;
    uint8_t version;
    MessageType type;
    uint32_t senderId;
    uint32_t sessionId;
    uint32_t sequence;
    PacketOrigin origin;        // Added for BLE routing
    uint32_t messageId;         // Unique message ID for deduplication
    uint32_t epoch;             // Game epoch for reconciliation
    uint32_t revision;          // State revision
};

// Wire format PacketHeader (20 bytes, no extensions)
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

struct PuzzleStatePacket {
    PacketHeader header;
    PuzzleState state;
};

struct PuzzleAckPacket {
    PacketHeader header;
    uint32_t targetBoardId;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
    MessageType acknowledgedType;
    uint8_t reserved[3];
};

struct StateRequestPacket {
    PacketHeader header;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
};

struct MastermindState {
    MastermindGuess guesses[kMastermindMaxGuesses];
    MastermindCode secret;
    uint32_t gameId;
    uint32_t revision;
    uint32_t hostBoardId;
    uint32_t guestBoardId;
    uint32_t codemakerBoardId;
    uint32_t roundWinnerBoardId;
    uint8_t hostScore;
    uint8_t guestScore;
    uint8_t round;
    uint8_t guessCount;
    MastermindPhase phase;
    uint8_t reserved[3];
};

struct GameStatePacket {
    PacketHeader header;
    MastermindState state;
};

struct GameAckPacket {
    PacketHeader header;
    uint32_t targetBoardId;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
    MessageType acknowledgedType;
    uint8_t reserved[3];
};

struct MastermindStateRequestPacket {
    PacketHeader header;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
};

// Active game clock
struct ActiveGameClock {
    uint32_t epoch;
    ActiveGameKind kind;
};

constexpr uint32_t kNoActiveGameEpoch = 0;

// Device info for BLE
struct DeviceInfo {
    uint32_t protocolVersion;
    uint32_t firmwareVersion;
    uint32_t boardId;
    uint8_t role; // 0=unknown, 1=host, 2=peer
    uint8_t activeGame;
    uint8_t featureFlags;
    uint8_t reserved[3];
};

// Puzzle utility functions
PuzzleSpec makePuzzleSpec(uint8_t columns, uint8_t rows, PuzzleTheme theme);
bool isSupportedPuzzleSpec(const PuzzleSpec& spec);
uint8_t puzzleTileCount(const PuzzleState& state);
PuzzleState makeInitialPuzzle(uint32_t firstTurnBoardId, uint32_t gameId, const PuzzleSpec& spec);
PuzzleState makeScrambledPuzzle(uint32_t firstTurnBoardId, uint32_t gameId, const PuzzleSpec& spec);
bool isSolvablePuzzleArrangement(const PuzzleState& state);
bool isValidPuzzle(const PuzzleState& state);
uint8_t findBlank(const PuzzleState& state);
bool isTileCorrect(const PuzzleState& state, uint8_t position);
bool isPuzzleSolved(const PuzzleState& state);
bool isSamePuzzle(const PuzzleState& first, const PuzzleState& second);
enum class PuzzleVersionOrder : uint8_t { Older, Same, Newer };
PuzzleVersionOrder comparePuzzleVersion(const PuzzleState& current, const PuzzleState& candidate);
uint32_t puzzleStateDigest(const PuzzleState& state);
bool isDeliverySuperseded(const PuzzleState& pending, const PuzzleState& observed);
enum class ReconciliationAction : uint8_t { None, SendFullState, RequestFullState };
ReconciliationAction decideReconciliation(const PuzzleState& local, uint32_t remoteGameId, uint32_t remoteRevision, uint32_t remoteDigest, bool localIsAuthority);
bool tryPuzzleMove(PuzzleState& state, uint8_t position, uint32_t actorBoardId, uint32_t nextTurnBoardId);
bool exitPuzzle(PuzzleState& state, uint32_t actorBoardId, uint32_t otherBoardId);
bool applyPuzzleExitSignal(PuzzleState& current, const PuzzleState& candidate, uint32_t senderBoardId, uint32_t recipientBoardId);
bool isValidRemoteTransition(const PuzzleState& current, const PuzzleState& incoming, uint32_t senderBoardId, uint32_t recipientBoardId);
bool isValidPuzzleForParticipants(const PuzzleState& state, uint32_t firstBoardId, uint32_t secondBoardId);
bool shouldAdoptFullState(const PuzzleState& current, const PuzzleState& incoming, uint32_t senderBoardId, uint32_t recipientBoardId);

// Mastermind utility functions
MastermindState makeMastermindMatch(uint32_t hostBoardId, uint32_t guestBoardId, uint32_t gameId);
bool isValidMastermindState(const MastermindState& state);
bool sameMastermindCode(const MastermindCode& first, const MastermindCode& second);
bool sameMastermindState(const MastermindState& first, const MastermindState& second);
MastermindFeedback evaluateMastermindGuess(const MastermindCode& secret, const MastermindCode& guess);
uint32_t mastermindCodebreakerBoardId(const MastermindState& state);
bool submitMastermindSecret(MastermindState& state, const MastermindCode& secret, uint32_t actorBoardId);
bool submitMastermindGuess(MastermindState& state, const MastermindCode& guess, uint32_t actorBoardId);
bool advanceMastermindRound(MastermindState& state, uint32_t actorBoardId);
bool exitMastermindMatch(MastermindState& state, uint32_t actorBoardId);
enum class MastermindVersionOrder : uint8_t { Older, Same, Newer };
MastermindVersionOrder compareMastermindVersion(const MastermindState& current, const MastermindState& candidate);
uint32_t mastermindStateDigest(const MastermindState& state);
bool shouldAdoptMastermindState(const MastermindState& current, const MastermindState& candidate);
bool isMastermindDeliverySuperseded(const MastermindState& pending, const MastermindState& observed);
bool applyMastermindExitSignal(MastermindState& current, const MastermindState& candidate, uint32_t senderBoardId);
bool isValidMastermindTransition(const MastermindState& current, const MastermindState& candidate, uint32_t senderBoardId);

} // namespace flip7