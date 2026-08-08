#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include <esp_now.h>
#include <esp_system.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cstring>

#include "game_selection.h"
#include "generated_peer_keys.h"
#include "protocol.h"
#include "puzzle_logic.h"
#include "flip7/countdown_engine.hpp"

namespace {
constexpr uint8_t kBacklightPin = 21;
constexpr uint8_t kTouchIrqPin = 36;
constexpr uint8_t kTouchMosiPin = 32;
constexpr uint8_t kTouchMisoPin = 39;
constexpr uint8_t kTouchClockPin = 25;
constexpr uint8_t kTouchCsPin = 33;
constexpr int16_t kTouchMinX = 200;
constexpr int16_t kTouchMaxX = 3700;
constexpr int16_t kTouchMinY = 240;
constexpr int16_t kTouchMaxY = 3800;
constexpr uint32_t kHeartbeatIntervalMs = 1000;
constexpr uint32_t kEspNowRetryMs = 2000;
constexpr uint32_t kDeliveryRetryMs = 300;
constexpr uint32_t kStateRequestIntervalMs = 1000;
constexpr uint32_t kPeerTimeoutMs = 3000;
constexpr uint32_t kCompleteDisplayMs = 2000;
constexpr uint32_t kMastermindCompleteDisplayMs = 3000;
constexpr size_t kRetiredSessionCapacity = 16;

struct KnownBoard {
    uint32_t id;
    uint8_t address[6];
};

constexpr KnownBoard kKnownBoards[] = {
    {0x6AF4E9D4, {0xD4, 0xE9, 0xF4, 0x6A, 0xF4, 0xFC}},
    {0xE7D8CBB0, {0xB0, 0xCB, 0xD8, 0xE7, 0x1E, 0xD4}},
};
constexpr int16_t kBoardAreaTop = 43;
constexpr int16_t kBoardAreaWidth = 304;
constexpr int16_t kBoardAreaHeight = 162;
constexpr int16_t kTileGap = 4;
constexpr int16_t kExitX = 271;
constexpr int16_t kExitY = 3;
constexpr int16_t kExitWidth = 46;
constexpr int16_t kExitHeight = 26;
constexpr int16_t kCountdownNewRoundX = 110;
constexpr int16_t kCountdownNewRoundY = 175;
constexpr int16_t kCountdownNewRoundWidth = 100;
constexpr int16_t kCountdownNewRoundHeight = 34;
constexpr int16_t kCountdownRoundTypeY = 90;
constexpr int16_t kCountdownRoundTypeWidth = 90;
constexpr int16_t kCountdownRoundTypeHeight = 60;
constexpr int16_t kCountdownNumbersX = 5;
constexpr int16_t kCountdownLettersX = 115;
constexpr int16_t kCountdownConundrumX = 225;

// In-round layout constants
constexpr int16_t kCdTimerCX = 280;  // arc centre x (top-right corner)
constexpr int16_t kCdTimerCY = 26;
constexpr int16_t kCdTimerR  = 22;
constexpr int16_t kCdTileY   = 48;   // tile strip top-left y
constexpr int16_t kCdTileH   = 34;
constexpr int16_t kCdTileW   = 46;
constexpr int16_t kCdTileGap = 4;
// Numpad (4 rows x 3 cols) — bottom half of screen
constexpr int16_t kCdPadX   = 48;    // left edge of pad
constexpr int16_t kCdPadY   = 105;
constexpr int16_t kCdPadW   = 70;
constexpr int16_t kCdPadH   = 32;
constexpr int16_t kCdPadGap = 4;
// Operation buttons row
constexpr int16_t kCdOpY   = 185;
constexpr int16_t kCdOpW   = 58;
constexpr int16_t kCdOpH   = 30;
// Large-count picker (0-4) for Numbers picking phase
constexpr int16_t kCdLcY   = 120;
constexpr int16_t kCdLcW   = 50;
constexpr int16_t kCdLcH   = 44;
// Admin FORCE-END button (host only, stalemate fallback)
constexpr int16_t kCdForceEndX = 230;
constexpr int16_t kCdForceEndY = 210;
constexpr int16_t kCdForceEndW = 80;
constexpr int16_t kCdForceEndH = 24;

struct PuzzleLayout {
    int16_t x;
    int16_t y;
    int16_t tileWidth;
    int16_t tileHeight;
};

constexpr uint16_t kMastermindColors[kMastermindColorCount + 1] = {
    TFT_DARKGREY, TFT_RED, TFT_ORANGE, TFT_YELLOW,
    TFT_GREEN, TFT_CYAN, TFT_MAGENTA,
};

TFT_eSPI display;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(kTouchCsPin, kTouchIrqPin);
portMUX_TYPE linkMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE gameMux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t protocolMutex = nullptr;

enum class ScreenMode : uint8_t { Home, Puzzle, Complete, Mastermind, Countdown };

struct LinkState {
    uint32_t sent;
    uint32_t sendFailures;
    uint32_t received;
    uint32_t lastReceivedMs;
    uint32_t peerBoardId;
    uint32_t peerSessionId;
    uint32_t retiredSessions[kRetiredSessionCapacity];
    size_t retiredSessionCount;
    uint32_t peerSequences[15];
    bool sequenceSeen[15];
    uint8_t peerAddress[6];
};

struct PendingDelivery {
    bool active;
    MessageType type;
    PuzzleState state;
    uint32_t lastSentMs;
};

struct PendingAck {
    bool active;
    uint32_t targetBoardId;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
    MessageType acknowledgedType;
};

struct MastermindPendingDelivery {
    bool active;
    MessageType type;
    MastermindState state;
    uint32_t lastSentMs;
};

struct MastermindPendingAck {
    bool active;
    uint32_t targetBoardId;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
    MessageType acknowledgedType;
};

struct CountdownPendingDelivery {
    bool active;
    MessageType type;
    CountdownWireState state;
    uint32_t lastSentMs;
};

struct CountdownPendingAck {
    bool active;
    uint32_t targetBoardId;
    uint32_t gameId;
    uint32_t revision;
    uint32_t stateDigest;
    MessageType acknowledgedType;
};

LinkState linkState{};
PuzzleState puzzleState{};
PendingDelivery pendingDelivery{};
PendingAck pendingAck{};
MastermindState mastermindState{};
MastermindPendingDelivery mastermindPendingDelivery{};
MastermindPendingAck mastermindPendingAck{};
MastermindCode draftCode{{1, 1, 1, 1}};
MastermindCode lastSubmittedGuess{{1, 1, 1, 1}};
flip7::countdown::CountdownMatchEngine countdownEngine(0, 0, 0);
CountdownWireState countdownState{};
CountdownPendingDelivery countdownPendingDelivery{};
CountdownPendingAck countdownPendingAck{};
ScreenMode screenMode = ScreenMode::Home;
ActiveGameClock activeGame{kNoActiveGameEpoch, ActiveGameKind::Home};
uint32_t boardId = 0;
uint32_t bootSessionId = 0;
uint32_t expectedPeerBoardId = 0;
uint8_t expectedPeerAddress[6]{};
uint32_t gamePeerBoardId = 0;
uint32_t nextSequence = 1;
uint32_t lastHeartbeatMs = 0;
uint32_t lastEspNowAttemptMs = 0;
uint32_t lastStateRequestMs = 0;
uint32_t completedAtMs = 0;
uint32_t mastermindCompletedAtMs = 0;
uint32_t remoteLogRevision = 0;
uint32_t remoteLogSender = 0;
uint32_t ackLogGameId = 0;
uint32_t ackLogRevision = 0;
uint32_t fullLogGameId = 0;
uint32_t fullLogRevision = 0;
bool espNowReady = false;
bool stateReady = false;
bool displayDirty = true;
bool requestStateSoon = false;
bool reconciliationPending = false;
bool sendFullStateSoon = false;
bool remoteLogPending = false;
bool ackLogPending = false;
bool fullLogPending = false;
bool mastermindStateReady = false;
bool mastermindRequestStateSoon = false;
bool mastermindReconciliationPending = false;
bool sendMastermindFullStateSoon = false;
bool countdownStateReady = false;
bool countdownRequestStateSoon = false;
bool countdownReconciliationPending = false;
bool sendCountdownFullStateSoon = false;

// Per-round UI state — local only, not transmitted.
struct NumUiState {
    int32_t  draftValue{0};     // numpad entry during ClaimEntry
    bool     claimSent{false};  // claim packet delivered to host
    uint8_t  largeCount{2};     // large-number count chosen in NumPicking
};
NumUiState numUi{};

struct LetUiState {
    uint8_t  claimLength{0};        // digit claim (1-9)
    bool     claimSent{false};
    char     word[10]{};            // word being built from tiles
    uint8_t  wordLen{0};
    bool     presentationSent{false};
    bool     verificationSent{false};
    bool     verificationAnswer{false};  // YES=true / NO=false
    bool     showConsonants{false};      // toggle in LetPicking view
    bool     presenterIsHost{true};      // which board presents first
};
LetUiState letUi{};

struct ConUiState {
    uint8_t  circleOrder[9]{0,1,2,3,4,5,6,7,8};  // display permutation
    uint8_t  selected[9]{};   // circleOrder indices tapped, in order
    uint8_t  selectedCount{0};
    bool     showGoodTry{false};
    uint32_t goodTryMs{0};
};
ConUiState conUi{};

uint32_t roundPhaseStartMs = 0;   // millis() when current sub-phase began
uint32_t lastAnimFrameMs   = 0;   // last millis() a timed animation redrawed
constexpr uint32_t kAnimFrameIntervalMs = 100;  // ~10 fps for the arc timer
bool touchWasDown = false;
bool lastOnline = false;
uint8_t selectedPeg = 0;
// MastermindPhase lastAdoptedMastermindPhase — reserved for future use

PacketHeader makeHeader(MessageType type) {
    return PacketHeader{kProtocolMagic, kProtocolVersion, type, boardId,
                        bootSessionId, nextSequence++};
}

bool validHeader(const PacketHeader& header, MessageType type) {
    return header.magic == kProtocolMagic &&
           header.version == kProtocolVersion && header.type == type &&
           header.senderId != 0 && header.senderId != boardId &&
           header.sessionId != 0;
}

bool sameAddress(const uint8_t* first, const uint8_t* second) {
    return memcmp(first, second, 6) == 0;
}

bool configureExpectedPeer() {
    for (const KnownBoard& local : kKnownBoards) {
        if (local.id != boardId) {
            continue;
        }
        for (const KnownBoard& candidate : kKnownBoards) {
            if (candidate.id == boardId) {
                continue;
            }
            expectedPeerBoardId = candidate.id;
            memcpy(expectedPeerAddress, candidate.address,
                   sizeof(expectedPeerAddress));
            return true;
        }
    }
    return false;
}

bool isExpectedPeer(const uint8_t* address, uint32_t senderId) {
    return senderId == expectedPeerBoardId &&
           sameAddress(address, expectedPeerAddress);
}

bool bindOrMatchPeer(const uint8_t* address, uint32_t senderId,
                     uint32_t sessionId, uint32_t sequence) {
    if (!isExpectedPeer(address, senderId)) {
        return false;
    }
    bool accepted = false;
    portENTER_CRITICAL(&linkMux);
    if (linkState.peerBoardId == 0) {
        linkState.peerBoardId = senderId;
        memcpy(linkState.peerAddress, address, sizeof(linkState.peerAddress));
    }
    const bool identityMatches = linkState.peerBoardId == senderId &&
                                 sameAddress(linkState.peerAddress, address);
    const bool peerWasOffline =
        linkState.received > 0 &&
        millis() - linkState.lastReceivedMs > kPeerTimeoutMs;
    const bool sessionChanges = linkState.peerSessionId != 0 &&
                                sessionId != linkState.peerSessionId;
    const bool sessionAllowed =
        identityMatches &&
        canUsePeerSession(linkState.peerSessionId, sessionId, peerWasOffline,
                          linkState.retiredSessions,
                          linkState.retiredSessionCount) &&
        (!sessionChanges ||
         linkState.retiredSessionCount < kRetiredSessionCapacity);
    if (sessionAllowed && sessionChanges) {
        linkState.retiredSessions[linkState.retiredSessionCount++] =
            linkState.peerSessionId;
        linkState.peerSessionId = sessionId;
        memset(linkState.sequenceSeen, 0, sizeof(linkState.sequenceSeen));
    } else if (sessionAllowed && linkState.peerSessionId == 0) {
        linkState.peerSessionId = sessionId;
    }

    const uint8_t index = static_cast<uint8_t>(MessageType::Heartbeat);
    const bool sequenceFresh =
        !linkState.sequenceSeen[index] ||
        isSequenceNewer(sequence, linkState.peerSequences[index]);
    if (sessionAllowed && sequenceFresh) {
        linkState.sequenceSeen[index] = true;
        linkState.peerSequences[index] = sequence;
        ++linkState.received;
        linkState.lastReceivedMs = millis();
        accepted = true;
    }
    portEXIT_CRITICAL(&linkMux);
    return accepted;
}

bool matchesBoundPeer(const uint8_t* address, uint32_t senderId) {
    bool matches = false;
    portENTER_CRITICAL(&linkMux);
    matches = linkState.peerBoardId == senderId && senderId != 0 &&
              sameAddress(linkState.peerAddress, address);
    portEXIT_CRITICAL(&linkMux);
    return matches;
}

bool acceptPeerSequence(MessageType type, uint32_t sessionId,
                        uint32_t sequence) {
    const uint8_t index = static_cast<uint8_t>(type);
    if (index >= 15) {
        return false;
    }
    portENTER_CRITICAL(&linkMux);
    const bool fresh = sessionId == linkState.peerSessionId &&
                       (!linkState.sequenceSeen[index] ||
                        isSequenceNewer(sequence,
                                        linkState.peerSequences[index]));
    if (fresh) {
        linkState.sequenceSeen[index] = true;
        linkState.peerSequences[index] = sequence;
    }
    portEXIT_CRITICAL(&linkMux);
    return fresh;
}

PuzzleLayout puzzleLayout(const PuzzleState& state) {
    const int16_t columns = state.columns;
    const int16_t rows = state.rows;
    const int16_t tileWidth = std::min<int16_t>(
        70, (kBoardAreaWidth - (columns - 1) * kTileGap) / columns);
    const int16_t tileHeight = std::min<int16_t>(
        50, (kBoardAreaHeight - (rows - 1) * kTileGap) / rows);
    const int16_t boardWidth =
        columns * tileWidth + (columns - 1) * kTileGap;
    const int16_t boardX = static_cast<int16_t>(
        (static_cast<int32_t>(display.width()) - boardWidth) / 2);
    return {boardX, kBoardAreaTop, tileWidth, tileHeight};
}

void drawThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                   uint16_t color) {
    display.drawLine(x0, y0, x1, y1, color);
    display.drawLine(x0 + 1, y0, x1 + 1, y1, color);
    display.drawLine(x0, y0 + 1, x1, y1 + 1, color);
}

void drawGreekSymbol(uint8_t symbol, int16_t cx, int16_t cy,
                     uint16_t color) {
    const int16_t left = cx - 14;
    const int16_t right = cx + 14;
    const int16_t top = cy - 16;
    const int16_t bottom = cy + 16;
    switch (symbol) {
        case 1:
            display.drawCircle(cx - 3, cy + 3, 10, color);
            display.drawCircle(cx - 3, cy + 3, 11, color);
            drawThickLine(cx + 7, top + 3, cx + 7, bottom, color);
            drawThickLine(cx + 7, cy + 4, right, cy - 8, color);
            break;
        case 2:
            drawThickLine(left + 5, top, left + 5, bottom, color);
            display.drawCircle(cx - 1, cy - 8, 9, color);
            display.drawCircle(cx - 1, cy + 9, 10, color);
            break;
        case 3:
            drawThickLine(left, top, right, top, color);
            drawThickLine(cx, top, cx, bottom, color);
            break;
        case 4:
            drawThickLine(cx, top, left, bottom, color);
            drawThickLine(cx, top, right, bottom, color);
            drawThickLine(left, bottom, right, bottom, color);
            break;
        case 5:
            drawThickLine(right, top, left, top, color);
            drawThickLine(left, top, left, bottom, color);
            drawThickLine(left, cy, cx + 8, cy, color);
            drawThickLine(left, bottom, right, bottom, color);
            break;
        case 6:
            drawThickLine(left, top, right, top, color);
            drawThickLine(right, top, left, bottom, color);
            drawThickLine(left, bottom, right, bottom, color);
            break;
        case 7:
            drawThickLine(left + 3, top, left + 3, bottom, color);
            drawThickLine(right - 3, top, right - 3, bottom, color);
            drawThickLine(left + 3, cy, right - 3, cy, color);
            break;
        case 8:
            display.drawEllipse(cx, cy, 13, 16, color);
            display.drawEllipse(cx, cy, 12, 15, color);
            drawThickLine(left + 3, cy, right - 3, cy, color);
            break;
        case 9:
            drawThickLine(cx, top, cx, bottom, color);
            drawThickLine(cx - 7, top, cx + 7, top, color);
            drawThickLine(cx - 7, bottom, cx + 7, bottom, color);
            break;
        case 10:
            drawThickLine(left + 3, top, left + 3, bottom, color);
            drawThickLine(left + 3, cy, right, top, color);
            drawThickLine(left + 3, cy, right, bottom, color);
            break;
        case 11:
            drawThickLine(cx, top, left, bottom, color);
            drawThickLine(cx, top, right, bottom, color);
            break;
        default:
            break;
    }
}

void drawPlanetSymbol(uint8_t planet, int16_t cx, int16_t cy,
                      uint16_t color) {
    switch (planet) {
        case 1:  // Mercury
            display.drawCircle(cx, cy - 2, 9, color);
            display.drawArc(cx, cy - 12, 8, 6, 200, 340, color, color);
            drawThickLine(cx, cy + 7, cx, cy + 16, color);
            drawThickLine(cx - 5, cy + 12, cx + 5, cy + 12, color);
            break;
        case 2:  // Venus
            display.drawCircle(cx, cy - 4, 10, color);
            display.drawCircle(cx, cy - 4, 11, color);
            drawThickLine(cx, cy + 7, cx, cy + 17, color);
            drawThickLine(cx - 6, cy + 12, cx + 6, cy + 12, color);
            break;
        case 3:  // Earth
            display.drawCircle(cx, cy, 13, color);
            display.drawCircle(cx, cy, 12, color);
            drawThickLine(cx - 11, cy, cx + 11, cy, color);
            drawThickLine(cx, cy - 11, cx, cy + 11, color);
            break;
        case 4:  // Mars
            display.drawCircle(cx - 3, cy + 3, 10, color);
            display.drawCircle(cx - 3, cy + 3, 11, color);
            drawThickLine(cx + 5, cy - 5, cx + 14, cy - 14, color);
            drawThickLine(cx + 8, cy - 14, cx + 14, cy - 14, color);
            drawThickLine(cx + 14, cy - 14, cx + 14, cy - 8, color);
            break;
        case 5:  // Jupiter
            drawThickLine(cx - 13, cy - 8, cx + 4, cy - 8, color);
            display.drawArc(cx - 3, cy - 1, 10, 8, 260, 80, color, color);
            drawThickLine(cx + 5, cy - 13, cx + 5, cy + 14, color);
            drawThickLine(cx - 4, cy + 7, cx + 13, cy + 7, color);
            break;
        case 6:  // Saturn
            drawThickLine(cx - 6, cy - 15, cx - 6, cy + 12, color);
            drawThickLine(cx - 13, cy - 7, cx + 4, cy - 7, color);
            display.drawArc(cx + 1, cy + 2, 9, 10, 260, 95, color, color);
            drawThickLine(cx, cy + 12, cx + 10, cy + 12, color);
            break;
        case 7:  // Uranus
            display.drawCircle(cx, cy + 3, 7, color);
            display.fillCircle(cx, cy + 3, 2, color);
            drawThickLine(cx, cy - 15, cx, cy - 4, color);
            drawThickLine(cx - 10, cy - 10, cx + 10, cy - 10, color);
            display.drawCircle(cx - 13, cy - 10, 3, color);
            display.drawCircle(cx + 13, cy - 10, 3, color);
            break;
        case 8:  // Neptune
            drawThickLine(cx, cy - 14, cx, cy + 14, color);
            drawThickLine(cx - 13, cy - 10, cx - 13, cy - 2, color);
            drawThickLine(cx + 13, cy - 10, cx + 13, cy - 2, color);
            display.drawArc(cx, cy - 3, 13, 11, 0, 180, color, color);
            drawThickLine(cx - 7, cy + 10, cx + 7, cy + 10, color);
            break;
        default:
            break;
    }
}

uint16_t tileColor(uint8_t tile) {
    constexpr uint16_t colors[] = {
        TFT_DARKCYAN, TFT_MAROON, TFT_DARKGREEN, TFT_PURPLE,
        TFT_OLIVE,    TFT_BLUE,   TFT_RED,       TFT_DARKCYAN,
        TFT_MAGENTA, TFT_ORANGE, TFT_GREEN,
    };
    return colors[(tile - 1) % 11];
}

bool peerOnline() {
    LinkState link{};
    portENTER_CRITICAL(&linkMux);
    link = linkState;
    portEXIT_CRITICAL(&linkMux);
    return link.received > 0 &&
           millis() - link.lastReceivedMs <= kPeerTimeoutMs;
}

bool localIsHost() {
    return gamePeerBoardId != 0 && boardId < gamePeerBoardId;
}

void renderLinkBadge(bool online) {
    display.setTextDatum(MC_DATUM);
    display.setTextColor(online ? TFT_GREEN : TFT_ORANGE, TFT_NAVY);
    display.drawString(online ? "LINKED" : "WAITING", 286, 31, 1);
}

void renderExitButton(bool online) {
    display.fillRoundRect(kExitX, kExitY, kExitWidth, kExitHeight, 5,
                          online ? TFT_RED : TFT_ORANGE);
    display.drawRoundRect(kExitX, kExitY, kExitWidth, kExitHeight, 5,
                          TFT_WHITE);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_WHITE, online ? TFT_RED : TFT_ORANGE);
    display.drawString("EXIT", kExitX + kExitWidth / 2,
                       kExitY + kExitHeight / 2, 2);
}

void renderHome(bool online, bool deliveryPending) {
    display.fillScreen(TFT_NAVY);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    display.drawString("TWO-PLAYER GAMES", display.width() / 2, 25, 4);
    renderLinkBadge(online);

    if (deliveryPending) {
        display.setTextColor(TFT_YELLOW, TFT_NAVY);
        display.drawString("RETURNING BOTH BOARDS HOME...",
                           display.width() / 2, 123, 2);
        return;
    }

    if (localIsHost() && online) {
        display.setTextColor(TFT_CYAN, TFT_NAVY);
        display.drawString("Choose a synchronized game", display.width() / 2,
                           55, 2);

        // Row 1: Slide puzzles
        display.fillRoundRect(10, 75, 145, 65, 8, TFT_PURPLE);
        display.drawRoundRect(10, 75, 145, 65, 8, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_PURPLE);
        display.drawString("PLANETS", 82, 95, 4);
        display.drawString("8 PIECES / 3x3", 82, 122, 2);

        display.fillRoundRect(165, 75, 145, 65, 8, TFT_DARKGREEN);
        display.drawRoundRect(165, 75, 145, 65, 8, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
        display.drawString("GREEK", 237, 95, 4);
        display.drawString("11 PIECES / 4x3", 237, 122, 2);

        // Row 2: Mastermind & Countdown
        display.fillRoundRect(10, 150, 145, 65, 8, TFT_MAROON);
        display.drawRoundRect(10, 150, 145, 65, 8, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_MAROON);
        display.drawString("MASTERMIND", 82, 170, 4);
        display.drawString("CODE BREAKER", 82, 197, 2);

        display.fillRoundRect(165, 150, 145, 65, 8, TFT_BLUE);
        display.drawRoundRect(165, 150, 145, 65, 8, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_BLUE);
        display.drawString("COUNTDOWN", 237, 170, 4);
        display.drawString("NUM/LET/CON", 237, 197, 2);
    } else {
        display.setTextColor(online ? TFT_LIGHTGREY : TFT_ORANGE, TFT_NAVY);
        display.drawString(online ? "WAITING FOR HOST" : "CONNECT PEER",
                           display.width() / 2, 125, 2);
    }
}

void renderPuzzle(bool online, const PuzzleState& game,
                  bool deliveryPending) {
    display.fillScreen(TFT_NAVY);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    display.drawString(game.theme == PuzzleTheme::Planets ? "PLANET SLIDE"
                                                          : "GREEK SLIDE",
                       145, 13, 4);
    renderExitButton(online);

    const PuzzleLayout layout = puzzleLayout(game);
    const uint8_t count = puzzleTileCount(game);
    for (uint8_t position = 0; position < count; ++position) {
        const int16_t column = position % game.columns;
        const int16_t row = position / game.columns;
        const int16_t x = layout.x + column * (layout.tileWidth + kTileGap);
        const int16_t y = layout.y + row * (layout.tileHeight + kTileGap);
        const uint8_t tile = game.tiles[position];
        if (tile == 0) {
            display.drawRoundRect(x, y, layout.tileWidth, layout.tileHeight, 7,
                                  TFT_DARKGREY);
            continue;
        }

        const bool locked = isTileCorrect(game, position);
        if (!locked) {
            display.fillRoundRect(x, y, layout.tileWidth, layout.tileHeight, 7,
                                  tileColor(tile));
            display.drawRoundRect(x, y, layout.tileWidth, layout.tileHeight, 7,
                                  TFT_WHITE);
            display.drawRoundRect(x + 1, y + 1, layout.tileWidth - 2,
                                  layout.tileHeight - 2, 6, TFT_WHITE);
        }
        const int16_t centerX = x + layout.tileWidth / 2;
        const int16_t centerY = y + layout.tileHeight / 2;
        const uint16_t symbolColor = locked ? TFT_GREEN : TFT_WHITE;
        if (game.theme == PuzzleTheme::Planets) {
            drawPlanetSymbol(tile, centerX, centerY, symbolColor);
        } else {
            drawGreekSymbol(tile, centerX, centerY, symbolColor);
        }
    }

    display.fillRect(0, 211, display.width(), 29, TFT_NAVY);
    display.setTextDatum(MC_DATUM);
    if (!online) {
        display.setTextColor(TFT_ORANGE, TFT_NAVY);
        display.drawString("WAITING FOR PEER", 125, 225, 2);
    } else if (deliveryPending) {
        display.setTextColor(TFT_YELLOW, TFT_NAVY);
        display.drawString("SYNCING MOVE...", 125, 225, 2);
    } else if (game.turnBoardId == boardId) {
        display.setTextColor(TFT_GREEN, TFT_NAVY);
        display.drawString("YOUR TURN", 125, 225, 2);
    } else {
        display.setTextColor(TFT_CYAN, TFT_NAVY);
        display.drawString("PEER TURN", 125, 225, 2);
    }
    char turnCount[20];
    snprintf(turnCount, sizeof(turnCount), "TURNS: %lu",
             static_cast<unsigned long>(game.revision));
    display.setTextDatum(MR_DATUM);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    display.drawString(turnCount, 315, 225, 2);
}

void renderComplete(bool online, const PuzzleState& game) {
    display.fillScreen(TFT_NAVY);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_GREEN, TFT_NAVY);
    display.drawString("PUZZLE COMPLETE!", display.width() / 2, 96, 4);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    display.drawString(game.theme == PuzzleTheme::Planets
                           ? "All eight planets are aligned"
                           : "All Greek symbols are home",
                       display.width() / 2, 138, 2);
    renderExitButton(online);
}

void drawMastermindCode(const MastermindCode& code, int16_t startX,
                        int16_t y, int16_t spacing, int16_t radius,
                        bool hidden = false) {
    for (uint8_t index = 0; index < kMastermindCodeLength; ++index) {
        const int16_t x = startX + index * spacing;
        const uint16_t color = hidden
                                   ? TFT_DARKGREY
                                   : kMastermindColors[code.colors[index]];
        display.fillCircle(x, y, radius, color);
        display.drawCircle(x, y, radius, TFT_WHITE);
    }
}

void renderMastermindHeader(const MastermindState& state, bool online) {
    display.fillRect(0, 0, 320, 32, TFT_NAVY);
    display.setTextDatum(ML_DATUM);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    char roundText[16];
    snprintf(roundText, sizeof(roundText), "ROUND %u", state.round);
    display.drawString(roundText, 7, 15, 2);
    display.setTextDatum(MC_DATUM);
    char scoreText[30];
    snprintf(scoreText, sizeof(scoreText), "HOST %u - %u GUEST",
             state.hostScore, state.guestScore);
    display.drawString(scoreText, 179, 15, 2);
    display.fillRoundRect(271, 3, 46, 26, 5,
                          online ? TFT_RED : TFT_ORANGE);
    display.drawRoundRect(271, 3, 46, 26, 5, TFT_WHITE);
    display.setTextColor(TFT_WHITE,
                         online ? TFT_RED : TFT_ORANGE);
    display.drawString("EXIT", 294, 16, 2);
}

void renderMastermindHistory(const MastermindState& state) {
    const uint8_t first = state.guessCount > 7 ? state.guessCount - 7 : 0;
    display.setTextDatum(ML_DATUM);
    for (uint8_t index = first; index < state.guessCount; ++index) {
        const int16_t y = 47 + (index - first) * 24;
        char number[5];
        snprintf(number, sizeof(number), "%u", index + 1);
        display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        display.drawString(number, 4, y, 2);
        drawMastermindCode(state.guesses[index].code, 30, y, 24, 8);
        char feedback[16];
        snprintf(feedback, sizeof(feedback), "E%u C%u",
                 state.guesses[index].feedback.exact,
                 state.guesses[index].feedback.colorOnly);
        display.drawString(feedback, 124, y, 2);
    }
}

void renderMastermind(bool online, const MastermindState& state,
                      bool deliveryPending) {
    display.fillScreen(TFT_BLACK);
    renderMastermindHeader(state, online);
    display.setTextDatum(MC_DATUM);
    if (state.phase == MastermindPhase::SecretEntry) {
        const bool localMaker = state.codemakerBoardId == boardId;
        display.setTextColor(TFT_YELLOW, TFT_BLACK);
        display.drawString(localMaker ? "SET YOUR SECRET CODE"
                                      : "OPPONENT IS SETTING A SECRET",
                           160, 48, 2);
        if (!localMaker) {
            MastermindCode hidden{};
            drawMastermindCode(hidden, 70, 110, 60, 18, true);
            display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
            display.drawString("Your guessing turn is next", 160, 166, 2);
            return;
        }
        drawMastermindCode(draftCode, 70, 88, 60, 18);
        display.drawCircle(70 + selectedPeg * 60, 88, 22, TFT_WHITE);
        for (uint8_t color = 1; color <= kMastermindColorCount; ++color) {
            const int16_t x = 45 + (color - 1) * 46;
            display.fillCircle(x, 143, 15, kMastermindColors[color]);
            display.drawCircle(x, 143, 15, TFT_WHITE);
        }
        display.fillRoundRect(95, 181, 130, 42, 7,
                              deliveryPending ? TFT_DARKGREY : TFT_GREEN);
        display.drawRoundRect(95, 181, 130, 42, 7, TFT_WHITE);
        display.setTextColor(TFT_BLACK,
                             deliveryPending ? TFT_DARKGREY : TFT_GREEN);
        display.drawString("CONFIRM", 160, 202, 4);
        return;
    }

    if (state.phase == MastermindPhase::RoundComplete) {
        const bool localWon = state.roundWinnerBoardId == boardId;
        display.setTextColor(localWon ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
        display.drawString(localWon ? "YOU WIN THE ROUND" : "OPPONENT WINS",
                           160, 62, 4);
        display.setTextColor(TFT_WHITE, TFT_BLACK);
        display.drawString("SECRET CODE", 160, 108, 2);
        drawMastermindCode(state.secret, 70, 143, 60, 17);
        display.setTextColor(TFT_CYAN, TFT_BLACK);
        display.drawString("ROLES SWAP NEXT ROUND", 160, 195, 2);
        return;
    }

    renderMastermindHistory(state);
    const bool localMaker = state.codemakerBoardId == boardId;
    display.drawFastVLine(194, 35, 202, TFT_DARKGREY);
    if (localMaker) {
        display.setTextColor(TFT_YELLOW, TFT_BLACK);
        display.drawString("YOUR SECRET", 257, 51, 2);
        drawMastermindCode(state.secret, 215, 86, 28, 10);
        display.setTextColor(TFT_CYAN, TFT_BLACK);
        display.drawString("OPPONENT", 257, 133, 2);
        display.drawString("IS GUESSING", 257, 153, 2);
        return;
    }
    display.setTextColor(TFT_YELLOW, TFT_BLACK);
    display.drawString("YOUR GUESS", 257, 45, 2);
    for (uint8_t color = 1; color <= kMastermindColorCount; ++color) {
        const int16_t x = 220 + ((color - 1) % 3) * 38;
        const int16_t y = 78 + ((color - 1) / 3) * 38;
        display.fillCircle(x, y, 13, kMastermindColors[color]);
        display.drawCircle(x, y, 13, TFT_WHITE);
    }
    drawMastermindCode(draftCode, 215, 157, 28, 10);
    display.drawCircle(215 + selectedPeg * 28, 157, 14, TFT_WHITE);
    display.fillRoundRect(211, 185, 92, 39, 6,
                          deliveryPending ? TFT_DARKGREY : TFT_GREEN);
    display.setTextColor(TFT_BLACK,
                         deliveryPending ? TFT_DARKGREY : TFT_GREEN);
    display.drawString("GUESS", 257, 204, 4);
}

// ---------------------------------------------------------------------------
// Countdown render helpers
// ---------------------------------------------------------------------------

// Shared header row: "ROUND N | HOST XX : GUEST XX | EXIT"
static void renderCountdownRoundHeader(const CountdownWireState& state,
                                       bool online) {
    display.fillRect(0, 0, 320, 36, TFT_NAVY);
    display.setTextDatum(ML_DATUM);
    display.setTextColor(TFT_LIGHTGREY, TFT_NAVY);
    char rlabel[20];
    snprintf(rlabel, sizeof(rlabel), "RD %lu",
             static_cast<unsigned long>(state.roundNumber));
    display.drawString(rlabel, 6, 18, 2);

    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    char scores[28];
    snprintf(scores, sizeof(scores), "HOST %d : %d GUEST",
             state.hostScore, state.guestScore);
    display.drawString(scores, 155, 18, 2);

    display.fillRoundRect(kExitX, kExitY, kExitWidth, kExitHeight, 5,
                          online ? TFT_RED : TFT_ORANGE);
    display.drawRoundRect(kExitX, kExitY, kExitWidth, kExitHeight, 5,
                          TFT_WHITE);
    display.setTextColor(TFT_WHITE, online ? TFT_RED : TFT_ORANGE);
    display.drawString("EXIT", kExitX + kExitWidth / 2,
                       kExitY + kExitHeight / 2, 2);
}

// Silent visual timer \u2014 procedural arc fallback (production will use MJPEG).
// Draws a ring from full to empty as elapsedMs grows toward durationMs.
static void renderCountdownAnimation(int16_t cx, int16_t cy, int16_t r,
                                     uint32_t elapsedMs, uint32_t durationMs) {
    // Background ring
    display.drawCircle(cx, cy, r, TFT_DARKGREY);
    display.drawCircle(cx, cy, r - 1, TFT_DARKGREY);

    float fraction = 1.0f - static_cast<float>(elapsedMs) /
                                 static_cast<float>(durationMs);
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    // Draw coloured arc (yellow > green; red when <20%)
    uint16_t colour = (fraction > 0.2f) ? TFT_YELLOW : TFT_RED;
    int segments = static_cast<int>(fraction * 60.0f);  // 60 = full circle
    for (int i = 0; i < segments; ++i) {
        float angle = static_cast<float>(i) * 6.0f - 90.0f;  // degrees
        float rad   = angle * 3.14159f / 180.0f;
        int16_t px  = cx + static_cast<int16_t>(r * cosf(rad));
        int16_t py  = cy + static_cast<int16_t>(r * sinf(rad));
        display.drawPixel(px, py, colour);
        px = cx + static_cast<int16_t>((r - 1) * cosf(rad));
        py = cy + static_cast<int16_t>((r - 1) * sinf(rad));
        display.drawPixel(px, py, colour);
    }

    // Remaining seconds label inside the ring
    uint32_t secsLeft = (durationMs > elapsedMs)
                            ? (durationMs - elapsedMs + 999) / 1000
                            : 0;
    display.setTextDatum(MC_DATUM);
    display.setTextColor(colour, TFT_NAVY);
    display.drawNumber(static_cast<int32_t>(secsLeft), cx, cy, 2);
}

// Draw the 6 Numbers tiles in a horizontal strip.
static void renderCountdownTileStrip(const flip7::countdown::NumbersRoundProjection& proj,
                                     uint16_t usedMask = 0) {
    for (uint8_t i = 0; i < proj.tileCount; ++i) {
        int16_t tx = static_cast<int16_t>(kCdTileGap +
                      i * (kCdTileW + kCdTileGap));
        bool used = (usedMask >> i) & 1u;
        uint16_t bg = used ? TFT_DARKGREY : TFT_DARKCYAN;
        display.fillRoundRect(tx, kCdTileY, kCdTileW, kCdTileH, 5, bg);
        display.drawRoundRect(tx, kCdTileY, kCdTileW, kCdTileH, 5, TFT_WHITE);
        display.setTextDatum(MC_DATUM);
        display.setTextColor(TFT_WHITE, bg);
        display.drawNumber(proj.tiles[i], tx + kCdTileW / 2,
                           kCdTileY + kCdTileH / 2, 2);
    }
}

// Draw 3x4 numpad for claim entry. Returns nothing; touch handler checks bounds.
static void renderCountdownNumpad(int32_t draftValue, bool submitted) {
    // Digit grid: [7][8][9] / [4][5][6] / [1][2][3] / [0][CLR][SUBMIT]
    const int digits[4][3] = {{7,8,9},{4,5,6},{1,2,3},{0,-1,-2}};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 3; ++col) {
            int16_t bx = static_cast<int16_t>(
                kCdPadX + col * (kCdPadW + kCdPadGap));
            int16_t by = static_cast<int16_t>(
                kCdPadY + row * (kCdPadH + kCdPadGap));
            int d = digits[row][col];
            uint16_t bg = TFT_DARKGREY;
            const char* label = "";
            char dbuf[4];
            if (d >= 0) {
                bg = TFT_DARKCYAN;
                snprintf(dbuf, sizeof(dbuf), "%d", d);
                label = dbuf;
            } else if (d == -1) {
                bg = TFT_MAROON;
                label = "CLR";
            } else {  // SUBMIT
                bg = submitted ? TFT_DARKGREY : TFT_DARKGREEN;
                label = "OK";
            }
            display.fillRoundRect(bx, by, kCdPadW, kCdPadH, 5, bg);
            display.drawRoundRect(bx, by, kCdPadW, kCdPadH, 5, TFT_WHITE);
            display.setTextDatum(MC_DATUM);
            display.setTextColor(TFT_WHITE, bg);
            display.drawString(label, bx + kCdPadW / 2, by + kCdPadH / 2, 2);
        }
    }
    // Draft value display above numpad
    display.fillRect(kCdPadX, kCdPadY - 36, 3 * kCdPadW + 2 * kCdPadGap, 32,
                     TFT_BLACK);
    display.drawRect(kCdPadX, kCdPadY - 36, 3 * kCdPadW + 2 * kCdPadGap, 32,
                     TFT_WHITE);
    display.setTextDatum(MR_DATUM);
    display.setTextColor(TFT_YELLOW, TFT_BLACK);
    char dvbuf[8];
    snprintf(dvbuf, sizeof(dvbuf), "%ld", static_cast<long>(draftValue));
    display.drawString(dvbuf,
                       kCdPadX + 3 * kCdPadW + 2 * kCdPadGap - 4,
                       kCdPadY - 20, 4);
}

// ---------------------------------------------------------------------------
// Round intro screen (Intro sub-phase) \u2014 all round types
// ---------------------------------------------------------------------------
static void renderCountdownIntro(const CountdownWireState& state,
                                 uint32_t elapsedMs, bool firstFrame) {
    if (firstFrame) {
        display.fillScreen(TFT_BLACK);
        display.setTextDatum(MC_DATUM);

        const char* title = "COUNTDOWN";
        uint16_t titleColour = TFT_WHITE;
        if (state.roundType == static_cast<uint8_t>(flip7::countdown::RoundType::Numbers)) {
            title = "NUMBERS ROUND";
            titleColour = TFT_CYAN;
        } else if (state.roundType == static_cast<uint8_t>(flip7::countdown::RoundType::Letters)) {
            title = "LETTERS ROUND";
            titleColour = TFT_YELLOW;
        } else if (state.roundType == static_cast<uint8_t>(flip7::countdown::RoundType::Conundrum)) {
            title = "CONUNDRUM ROUND";
            titleColour = TFT_MAGENTA;
        }
        display.setTextColor(titleColour, TFT_BLACK);
        display.drawString(title, 160, 75, 4);
        display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        display.drawString("Get ready...", 160, 130, 2);
    }
    // Always update the timer circle region only
    display.fillCircle(160, 175, 33, TFT_BLACK);
    renderCountdownAnimation(160, 175, 30, elapsedMs, 5000);
}

// ---------------------------------------------------------------------------
// Numbers sub-phase renders
// ---------------------------------------------------------------------------

static void renderCountdownNumPicking(const CountdownWireState& state,
                                      uint8_t selectedLarge) {
    display.fillScreen(TFT_BLACK);
    renderCountdownRoundHeader(state, false);

    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("HOW MANY LARGE NUMBERS?", 160, 55, 2);
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.drawString("Large: 25 50 75 100   Small: 1-10", 160, 75, 2);

    // 5 buttons: 0 1 2 3 4
    for (uint8_t i = 0; i <= 4; ++i) {
        int16_t bx = static_cast<int16_t>(16 + i * 58);
        uint16_t bg = (i == selectedLarge) ? TFT_CYAN : TFT_DARKGREY;
        uint16_t fg = (i == selectedLarge) ? TFT_BLACK : TFT_WHITE;
        display.fillRoundRect(bx, kCdLcY, kCdLcW, kCdLcH, 8, bg);
        display.drawRoundRect(bx, kCdLcY, kCdLcW, kCdLcH, 8, TFT_WHITE);
        display.setTextColor(fg, bg);
        display.drawNumber(i, bx + kCdLcW / 2, kCdLcY + kCdLcH / 2, 4);
    }

    if (boardId == state.chooserBoardId) {
        display.fillRoundRect(95, 185, 130, 36, 8, TFT_DARKGREEN);
        display.drawRoundRect(95, 185, 130, 36, 8, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
        display.drawString("CONFIRM", 160, 203, 4);
    } else {
        display.setTextColor(TFT_YELLOW, TFT_BLACK);
        display.drawString("Waiting for chooser...", 160, 200, 2);
    }
}

static void renderCountdownNumThinking(const CountdownWireState& state,
                                       const flip7::countdown::NumbersRoundProjection& proj,
                                       uint32_t elapsedMs, bool firstFrame) {
    if (firstFrame) {
        display.fillScreen(TFT_BLACK);
        renderCountdownRoundHeader(state, false);
        renderCountdownTileStrip(proj);

        display.setTextDatum(MC_DATUM);
        display.setTextColor(TFT_CYAN, TFT_BLACK);
        display.drawString("TARGET", 130, 100, 2);
        display.setTextColor(TFT_WHITE, TFT_BLACK);
        display.drawNumber(static_cast<int32_t>(proj.target), 130, 135, 6);

        display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        display.drawString("Calculate on paper", 130, 190, 2);
    }
    // Only update the timer circle region to avoid full-screen flicker
    display.fillCircle(kCdTimerCX, kCdTimerCY, kCdTimerR + 3, TFT_BLACK);
    renderCountdownAnimation(kCdTimerCX, kCdTimerCY, kCdTimerR,
                             elapsedMs, 30000);
}

static void renderCountdownNumClaimEntry(const CountdownWireState& state,
                                         const flip7::countdown::NumbersRoundProjection& proj,
                                         int32_t draftValue, bool claimSent) {
    display.fillScreen(TFT_BLACK);
    renderCountdownRoundHeader(state, false);
    renderCountdownTileStrip(proj);

    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("TARGET", 75, 100, 2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawNumber(static_cast<int32_t>(proj.target), 75, 130, 4);

    if (claimSent) {
        display.setTextColor(TFT_GREEN, TFT_BLACK);
        display.drawString("Claim sent!", 75, 165, 2);
        display.drawString("Waiting for opponent...", 75, 185, 2);
    } else {
        display.setTextColor(TFT_YELLOW, TFT_BLACK);
        display.drawString("Your answer:", 75, 100 + 3, 2);
        renderCountdownNumpad(draftValue, claimSent);
    }
}

static void renderCountdownNumClaimReveal(const CountdownWireState& state,
                                           const flip7::countdown::NumbersRoundProjection& proj,
                                           int32_t hostClaim, int32_t guestClaim) {
    display.fillScreen(TFT_BLACK);
    renderCountdownRoundHeader(state, false);

    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("CLAIMS REVEALED", 160, 52, 4);

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("TARGET", 160, 90, 2);
    display.drawNumber(static_cast<int32_t>(proj.target), 160, 115, 4);

    display.setTextColor(TFT_YELLOW, TFT_BLACK);
    char hbuf[20], gbuf[20];
    snprintf(hbuf, sizeof(hbuf), "HOST:  %ld", static_cast<long>(hostClaim));
    snprintf(gbuf, sizeof(gbuf), "GUEST: %ld", static_cast<long>(guestClaim));
    display.drawString(hbuf, 160, 155, 4);
    display.drawString(gbuf, 160, 190, 4);
}

static void renderCountdownNumPresenting(const CountdownWireState& state,
                                          const flip7::countdown::NumbersRoundProjection& proj,
                                          bool isPresenter) {
    display.fillScreen(TFT_BLACK);
    renderCountdownRoundHeader(state, false);
    renderCountdownTileStrip(proj);

    display.setTextDatum(MC_DATUM);

    if (!isPresenter) {
        display.setTextColor(TFT_YELLOW, TFT_BLACK);
        display.drawString("Opponent is presenting...", 160, 130, 2);
        return;
    }

    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("TARGET", 160, 96, 2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawNumber(static_cast<int32_t>(proj.target), 160, 120, 4);

    // Operation buttons
    const char* ops[4] = {"+", "-", "x", "/"};
    for (int i = 0; i < 4; ++i) {
        int16_t bx = static_cast<int16_t>(6 + i * (kCdOpW + 6));
        display.fillRoundRect(bx, kCdOpY, kCdOpW, kCdOpH, 6, TFT_DARKGREY);
        display.drawRoundRect(bx, kCdOpY, kCdOpW, kCdOpH, 6, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_DARKGREY);
        display.drawString(ops[i], bx + kCdOpW / 2, kCdOpY + kCdOpH / 2, 4);
    }

    // UNDO, END buttons
    display.fillRoundRect(256, kCdOpY, 58, kCdOpH, 6, TFT_MAROON);
    display.setTextColor(TFT_WHITE, TFT_MAROON);
    display.drawString("UNDO", 285, kCdOpY + kCdOpH / 2, 2);

    display.fillRoundRect(256, kCdOpY + kCdOpH + 6, 58, kCdOpH, 6,
                          TFT_DARKGREEN);
    display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    display.drawString("END", 285, kCdOpY + kCdOpH + 6 + kCdOpH / 2, 2);
}

// ---------------------------------------------------------------------------
// BetweenRounds scoreboard
// ---------------------------------------------------------------------------
static void renderCountdownBetweenRounds(bool online,
                                          const CountdownWireState& state) {
    display.fillScreen(TFT_NAVY);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    renderExitButton(online);

    display.drawString("CHOOSE NEXT ROUND", 145, 20, 4);

    // Show both roles when they differ
    bool isChooser = (boardId == state.chooserBoardId);
    bool isHost    = (boardId == state.hostBoardId);
    display.setTextColor(TFT_YELLOW, TFT_NAVY);
    if (isChooser) {
        display.drawString("YOUR TURN TO CHOOSE", 160, 55, 2);
    } else if (isHost) {
        display.drawString("WAITING FOR PEER TO CHOOSE", 160, 55, 2);
    } else {
        display.drawString("WAITING FOR CHOOSER", 160, 55, 2);
    }

    if (isChooser) {
        // NUMBERS button
        display.fillRoundRect(kCountdownNumbersX, kCountdownRoundTypeY,
                              kCountdownRoundTypeWidth, kCountdownRoundTypeHeight,
                              8, TFT_DARKCYAN);
        display.drawRoundRect(kCountdownNumbersX, kCountdownRoundTypeY,
                              kCountdownRoundTypeWidth, kCountdownRoundTypeHeight,
                              8, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_DARKCYAN);
        display.drawString("NUMBERS",
                           kCountdownNumbersX + kCountdownRoundTypeWidth / 2,
                           kCountdownRoundTypeY + kCountdownRoundTypeHeight / 2,
                           2);
        // LETTERS button
        display.fillRoundRect(kCountdownLettersX, kCountdownRoundTypeY,
                              kCountdownRoundTypeWidth, kCountdownRoundTypeHeight,
                              8, TFT_YELLOW);
        display.drawRoundRect(kCountdownLettersX, kCountdownRoundTypeY,
                              kCountdownRoundTypeWidth, kCountdownRoundTypeHeight,
                              8, TFT_WHITE);
        display.setTextColor(TFT_BLACK, TFT_YELLOW);
        display.drawString("LETTERS",
                           kCountdownLettersX + kCountdownRoundTypeWidth / 2,
                           kCountdownRoundTypeY + kCountdownRoundTypeHeight / 2,
                           2);
        // CONUNDRUM button
        display.fillRoundRect(kCountdownConundrumX, kCountdownRoundTypeY,
                              kCountdownRoundTypeWidth, kCountdownRoundTypeHeight,
                              8, TFT_MAGENTA);
        display.drawRoundRect(kCountdownConundrumX, kCountdownRoundTypeY,
                              kCountdownRoundTypeWidth, kCountdownRoundTypeHeight,
                              8, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_MAGENTA);
        display.drawString("CONUNDRUM",
                           kCountdownConundrumX + kCountdownRoundTypeWidth / 2,
                           kCountdownRoundTypeY + kCountdownRoundTypeHeight / 2,
                           2);
    }

    // Scores + round number
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    char hostLabel[20], guestLabel[20];
    snprintf(hostLabel,  sizeof(hostLabel),  "HOST  %d", state.hostScore);
    snprintf(guestLabel, sizeof(guestLabel), "GUEST %d", state.guestScore);
    display.drawString(hostLabel,  80,  185, 4);
    display.drawString(guestLabel, 240, 185, 4);

    char roundLabel[24];
    snprintf(roundLabel, sizeof(roundLabel), "Round %lu",
             static_cast<unsigned long>(state.roundNumber));
    display.drawString(roundLabel, 160, 220, 2);
}

// ---------------------------------------------------------------------------
// Main renderCountdown dispatcher
// ---------------------------------------------------------------------------
// Forward declaration — renderCountdownLetConPhases is defined after the helpers.
static void renderCountdownLetConPhases(
    bool online, const CountdownWireState& state,
    CountdownRoundSubPhase subPhase, uint32_t elapsed);

void renderCountdown(bool online, const CountdownWireState& state) {
    using SubPhase = CountdownRoundSubPhase;
    auto subPhase  = static_cast<SubPhase>(state.roundSubPhase);
    uint32_t now   = millis();
    uint32_t elapsed = (now >= roundPhaseStartMs)
                           ? now - roundPhaseStartMs
                           : 0;

    if (state.phase == CountdownWirePhase::BetweenRounds ||
        state.phase == CountdownWirePhase::Setup) {
        renderCountdownBetweenRounds(online, state);
        return;
    }

    // Intro sub-phase \u2014 common to all round types
    if (subPhase == SubPhase::Intro) {
        renderCountdownIntro(state, elapsed,
                             lastAnimFrameMs < roundPhaseStartMs);
        return;
    }

    // Numbers sub-phases
    if (subPhase == SubPhase::NumPicking) {
        renderCountdownNumPicking(state, numUi.largeCount);
        return;
    }
    if (subPhase == SubPhase::NumThinking) {
        flip7::countdown::NumbersRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.numbersProjection();
        portEXIT_CRITICAL(&gameMux);
        renderCountdownNumThinking(state, proj, elapsed,
                                   lastAnimFrameMs < roundPhaseStartMs);
        return;
    }
    if (subPhase == SubPhase::NumClaimEntry) {
        flip7::countdown::NumbersRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.numbersProjection();
        portEXIT_CRITICAL(&gameMux);
        renderCountdownNumClaimEntry(state, proj, numUi.draftValue,
                                     numUi.claimSent);
        return;
    }
    if (subPhase == SubPhase::NumClaimReveal) {
        flip7::countdown::NumbersRoundProjection proj{};
        int32_t hClaim = 0, gClaim = 0;
        portENTER_CRITICAL(&gameMux);
        proj   = countdownEngine.numbersProjection();
        auto* nr = countdownEngine.numbersRound();
        if (nr) {
            hClaim = nr->claimFor(state.hostBoardId);
            gClaim = nr->claimFor(state.guestBoardId);
        }
        portEXIT_CRITICAL(&gameMux);
        renderCountdownNumClaimReveal(state, proj, hClaim, gClaim);
        return;
    }
    if (subPhase == SubPhase::NumPresentPlayerA ||
        subPhase == SubPhase::NumPresentPlayerB) {
        flip7::countdown::NumbersRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.numbersProjection();
        portEXIT_CRITICAL(&gameMux);
        // Determine if this board is the active presenter
        // (simple heuristic: PlayerA = host, PlayerB = guest for now)
        bool isPresenterA = (boardId == state.hostBoardId);
        bool isActive = (subPhase == SubPhase::NumPresentPlayerA && isPresenterA) ||
                        (subPhase == SubPhase::NumPresentPlayerB && !isPresenterA);
        renderCountdownNumPresenting(state, proj, isActive);
        return;
    }
    if (subPhase == SubPhase::NumResult) {
        display.fillScreen(TFT_BLACK);
        renderCountdownRoundHeader(state, online);
        display.setTextDatum(MC_DATUM);
        display.setTextColor(TFT_GREEN, TFT_BLACK);
        display.drawString("ROUND COMPLETE", 160, 120, 4);
        display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        display.drawString("Next round starting...", 160, 175, 2);
        return;
    }

    // Letters and Conundrum phases — defined after helpers below.
    renderCountdownLetConPhases(online, state, subPhase, elapsed);
}

// ---------------------------------------------------------------------------
// Letters render helpers
// ---------------------------------------------------------------------------

// 9-letter strip reused across LetThinking, LetPresenting, LetResult.
// tappable=true draws each tile as a pressable button.
// usedMask: bit N set = tile N dimmed (already placed in word).
static void renderLetterStrip(const char* letters, uint8_t count,
                               bool tappable = false,
                               uint16_t usedMask = 0) {
    constexpr int16_t W = 30, H = 34, GAP = 4;
    int16_t startX = static_cast<int16_t>((320 - count * (W + GAP) + GAP) / 2);
    for (uint8_t i = 0; i < count; ++i) {
        int16_t tx = static_cast<int16_t>(startX + i * (W + GAP));
        bool used   = tappable && ((usedMask >> i) & 1u);
        uint16_t bg = used ? TFT_DARKGREY
                           : (tappable ? TFT_DARKCYAN : 0x1082 /*dark navy*/);
        display.fillRoundRect(tx, kCdTileY, W, H, 5, bg);
        display.drawRoundRect(tx, kCdTileY, W, H, 5, TFT_WHITE);
        display.setTextDatum(MC_DATUM);
        display.setTextColor(TFT_WHITE, bg);
        char s[2] = {letters[i], '\0'};
        display.drawString(s, tx + W / 2, kCdTileY + H / 2, 2);
    }
}

// Vowel button row (A E I O U) for LetPicking.
static void renderVowelButtons(uint8_t vowelCount) {
    constexpr int16_t W = 50, H = 44, Y = 95;
    const char vowels[5] = {'A', 'E', 'I', 'O', 'U'};
    for (int i = 0; i < 5; ++i) {
        int16_t bx = static_cast<int16_t>(10 + i * 60);
        uint16_t bg = (vowelCount >= 5) ? TFT_DARKGREY : TFT_DARKCYAN;
        display.fillRoundRect(bx, Y, W, H, 8, bg);
        display.drawRoundRect(bx, Y, W, H, 8, TFT_WHITE);
        display.setTextDatum(MC_DATUM);
        display.setTextColor(TFT_WHITE, bg);
        char s[2] = {vowels[i], '\0'};
        display.drawString(s, bx + W / 2, Y + H / 2, 4);
    }
}

// Consonant keyboard (4 rows) for LetPicking.
static void renderConsonantKeyboard(uint8_t consonantCount) {
    // BCDFGH / JKLMNP / QRSTVW / XYZ
    const char rows[4][7] = {
        {'B','C','D','F','G','H', 0},
        {'J','K','L','M','N','P', 0},
        {'Q','R','S','T','V','W', 0},
        {'X','Y','Z', 0, 0, 0,  0},
    };
    const int rowLen[4] = {6, 6, 6, 3};
    constexpr int16_t W = 42, H = 32, GAP = 4;
    bool maxed = (consonantCount >= 6);
    for (int row = 0; row < 4; ++row) {
        int16_t startX = static_cast<int16_t>(
            (320 - rowLen[row] * (W + GAP) + GAP) / 2);
        int16_t by = static_cast<int16_t>(88 + row * (H + GAP));
        for (int col = 0; col < rowLen[row]; ++col) {
            int16_t bx = static_cast<int16_t>(startX + col * (W + GAP));
            uint16_t bg = maxed ? TFT_DARKGREY : TFT_DARKGREY;
            // Consonants always dark-grey unless enabled — keep simple
            bg = maxed ? 0x2104 /*very dark*/ : TFT_DARKGREY;
            display.fillRoundRect(bx, by, W, H, 5, bg);
            display.drawRoundRect(bx, by, W, H, 5, TFT_WHITE);
            display.setTextDatum(MC_DATUM);
            display.setTextColor(TFT_WHITE, bg);
            char s[2] = {rows[row][col], '\0'};
            display.drawString(s, bx + W / 2, by + H / 2, 2);
        }
    }
}

static void renderCountdownLetPicking(const CountdownWireState& state,
                                       const flip7::countdown::LettersRoundProjection& proj,
                                       bool showConsonants) {
    display.fillScreen(TFT_BLACK);
    renderCountdownRoundHeader(state, false);

    bool isChooser = (boardId == state.chooserBoardId);

    // Drawn-letters progress strip
    if (proj.letterCount > 0)
        renderLetterStrip(proj.letters, proj.letterCount);

    display.setTextDatum(MC_DATUM);
    if (!isChooser) {
        display.setTextColor(TFT_YELLOW, TFT_BLACK);
        display.drawString("Waiting for chooser...", 160, 130, 2);
        return;
    }

    // Toggle tabs: [VOWELS] [CONSONANTS]
    uint16_t vbg = showConsonants ? TFT_DARKGREY : TFT_DARKCYAN;
    uint16_t cbg = showConsonants ? TFT_DARKGREY : TFT_DARKGREY;
    // Actually light up the active tab
    vbg = !showConsonants ? TFT_DARKCYAN : TFT_DARKGREY;
    cbg =  showConsonants ? TFT_DARKGREY : TFT_DARKGREY;
    display.fillRoundRect(5, 50, 100, 28, 6, vbg);
    display.setTextColor(TFT_WHITE, vbg);
    display.drawString("VOWELS", 55, 64, 2);
    display.fillRoundRect(215, 50, 100, 28, 6, cbg);
    display.setTextColor(TFT_WHITE, cbg);
    display.drawString("CONSONANTS", 265, 64, 2);

    // Progress label
    char prog[36];
    snprintf(prog, sizeof(prog), "V:%u/4  C:%u/5  Total:%u/9",
             static_cast<unsigned>(proj.letterCount),  // rough
             0u, static_cast<unsigned>(proj.letterCount));
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.drawString(prog, 160, 83, 2);

    if (!showConsonants) {
        // Count vowels in drawn set
        uint8_t vc = 0;
        for (uint8_t i = 0; i < proj.letterCount; ++i) {
            char c = proj.letters[i];
            if (c=='A'||c=='E'||c=='I'||c=='O'||c=='U') ++vc;
        }
        renderVowelButtons(vc);
    } else {
        uint8_t cc = 0;
        for (uint8_t i = 0; i < proj.letterCount; ++i) {
            char c = proj.letters[i];
            if (c!='A'&&c!='E'&&c!='I'&&c!='O'&&c!='U') ++cc;
        }
        renderConsonantKeyboard(cc);
    }

    if (proj.letterCount >= 9) {
        display.fillRoundRect(95, 200, 130, 34, 8, TFT_DARKGREEN);
        display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
        display.drawString("LOCK IN", 160, 217, 4);
    }
}

static void renderCountdownLetThinking(const CountdownWireState& state,
                                        const flip7::countdown::LettersRoundProjection& proj,
                                        uint32_t elapsedMs, bool firstFrame) {
    if (firstFrame) {
        display.fillScreen(TFT_BLACK);
        renderCountdownRoundHeader(state, false);
        renderLetterStrip(proj.letters, proj.letterCount);

        display.setTextDatum(MC_DATUM);
        display.setTextColor(TFT_YELLOW, TFT_BLACK);
        display.drawString("Make the longest word you can", 160, 100, 2);
        display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        display.drawString("Your time starts now", 160, 120, 2);
    }
    display.fillCircle(kCdTimerCX, kCdTimerCY, kCdTimerR + 3, TFT_BLACK);
    renderCountdownAnimation(kCdTimerCX, kCdTimerCY, kCdTimerR,
                             elapsedMs, 30000);
}

static void renderCountdownLetClaimEntry(const CountdownWireState& state,
                                          const flip7::countdown::LettersRoundProjection& proj,
                                          uint8_t selectedLen, bool sent) {
    display.fillScreen(TFT_BLACK);
    renderCountdownRoundHeader(state, false);
    renderLetterStrip(proj.letters, proj.letterCount);

    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_YELLOW, TFT_BLACK);
    display.drawString("Longest word you found:", 160, 95, 2);
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.drawString("Keep your claim private", 160, 113, 2);

    if (sent) {
        display.setTextColor(TFT_GREEN, TFT_BLACK);
        display.drawString("Claim sent!", 160, 155, 4);
        display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        display.drawString("Waiting for opponent...", 160, 190, 2);
        return;
    }

    // 9 digit buttons (1-9) in a row
    constexpr int16_t DW = 28, DH = 34, DY = 130, DGAP = 3;
    int16_t startX = static_cast<int16_t>((320 - 9*(DW+DGAP)+DGAP) / 2);
    for (int d = 1; d <= 9; ++d) {
        int16_t bx = static_cast<int16_t>(startX + (d-1)*(DW+DGAP));
        uint16_t bg = (selectedLen == d) ? TFT_CYAN : TFT_DARKGREY;
        uint16_t fg = (selectedLen == d) ? TFT_BLACK : TFT_WHITE;
        display.fillRoundRect(bx, DY, DW, DH, 5, bg);
        display.drawRoundRect(bx, DY, DW, DH, 5, TFT_WHITE);
        display.setTextDatum(MC_DATUM);
        display.setTextColor(fg, bg);
        display.drawNumber(d, bx + DW/2, DY + DH/2, 2);
    }

    uint16_t sbg = (selectedLen > 0) ? TFT_DARKGREEN : TFT_DARKGREY;
    display.fillRoundRect(95, 180, 130, 34, 8, sbg);
    display.drawRoundRect(95, 180, 130, 34, 8, TFT_WHITE);
    display.setTextColor(TFT_WHITE, sbg);
    display.drawString("SUBMIT CLAIM", 160, 197, 2);
}

static void renderCountdownLetClaimReveal(const CountdownWireState& state,
                                           const flip7::countdown::LettersRoundProjection& proj,
                                           uint8_t hostClaim, uint8_t guestClaim) {
    display.fillScreen(TFT_BLACK);
    renderCountdownRoundHeader(state, false);
    renderLetterStrip(proj.letters, proj.letterCount);

    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("CLAIMS REVEALED", 160, 58, 4);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    char hbuf[24], gbuf[24];
    snprintf(hbuf, sizeof(hbuf), "HOST:  %u letters", hostClaim);
    snprintf(gbuf, sizeof(gbuf), "GUEST: %u letters", guestClaim);
    display.drawString(hbuf, 160, 120, 4);
    display.drawString(gbuf, 160, 158, 4);

    uint8_t bigger = (hostClaim >= guestClaim) ? hostClaim : guestClaim;
    bool hostFirst = (hostClaim >= guestClaim);
    display.setTextColor(TFT_YELLOW, TFT_BLACK);
    char who[24];
    snprintf(who, sizeof(who), "%s presents first",
             hostFirst ? "HOST" : "GUEST");
    display.drawString(who, 160, 195, 2);
    (void)bigger;
}

static void renderCountdownLetPresenting(const CountdownWireState& state,
                                          const flip7::countdown::LettersRoundProjection& proj,
                                          bool isPresenter,
                                          const char* word, uint8_t wordLen,
                                          bool sent) {
    display.fillScreen(TFT_BLACK);
    renderCountdownRoundHeader(state, false);

    // Build used-letter bitmask: count letters in word vs available
    uint16_t usedMask = 0;
    if (isPresenter && wordLen > 0) {
        char pool[10]{};
        memcpy(pool, proj.letters, proj.letterCount);
        for (uint8_t wi = 0; wi < wordLen; ++wi) {
            for (uint8_t ti = 0; ti < proj.letterCount; ++ti) {
                if (!(usedMask >> ti & 1u) && pool[ti] == word[wi]) {
                    usedMask |= (1u << ti);
                    break;
                }
            }
        }
    }
    renderLetterStrip(proj.letters, proj.letterCount, isPresenter && !sent,
                      usedMask);

    display.setTextDatum(MC_DATUM);
    if (!isPresenter) {
        display.setTextColor(TFT_YELLOW, TFT_BLACK);
        display.drawString("Opponent is presenting...", 160, 110, 2);
        if (wordLen > 0) {
            // Show word being built in real-time (updated via wire)
            char wbuf[12]{};
            memcpy(wbuf, word, wordLen);
            display.setTextColor(TFT_WHITE, TFT_BLACK);
            display.drawString(wbuf, 160, 150, 4);
        }
        return;
    }

    // Word strip (building)
    char wbuf[12]{};
    memcpy(wbuf, word, wordLen);
    display.fillRect(0, 95, 320, 40, TFT_BLACK);
    display.drawRect(10, 98, 230, 34, TFT_DARKGREY);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextDatum(ML_DATUM);
    display.drawString(wordLen > 0 ? wbuf : "_", 18, 115, 4);
    display.setTextDatum(MC_DATUM);

    if (!sent) {
        // CLEAR and PRESENT buttons
        display.fillRoundRect(250, 98, 64, 34, 6, TFT_MAROON);
        display.setTextColor(TFT_WHITE, TFT_MAROON);
        display.drawString("CLR", 282, 115, 2);

        uint16_t pbg = (wordLen > 0) ? TFT_DARKGREEN : TFT_DARKGREY;
        display.fillRoundRect(95, 155, 130, 34, 8, pbg);
        display.drawRoundRect(95, 155, 130, 34, 8, TFT_WHITE);
        display.setTextColor(TFT_WHITE, pbg);
        display.drawString("PRESENT WORD", 160, 172, 2);
    } else {
        display.setTextColor(TFT_GREEN, TFT_BLACK);
        display.drawString("Waiting for verification...", 160, 175, 2);
    }
}

static void renderCountdownLetVerification(const CountdownWireState& state,
                                             const char* word, uint8_t wordLen,
                                             bool isVerifier, bool sent) {
    display.fillScreen(TFT_BLACK);
    renderCountdownRoundHeader(state, false);

    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString(isVerifier ? "VERIFY WORD" : "Waiting for verification...",
                       160, 55, 4);

    // Show the word letter by letter in boxes
    if (wordLen > 0) {
        constexpr int16_t W = 30, H = 38, GAP = 4;
        int16_t startX = static_cast<int16_t>(
            (320 - wordLen * (W + GAP) + GAP) / 2);
        display.setTextColor(TFT_WHITE, 0x18C3 /*dark teal*/);
        for (uint8_t i = 0; i < wordLen; ++i) {
            int16_t bx = static_cast<int16_t>(startX + i * (W + GAP));
            display.fillRect(bx, 100, W, H, 0x18C3);
            display.drawRect(bx, 100, W, H, TFT_WHITE);
            char s[2] = {word[i], '\0'};
            display.drawString(s, bx + W/2, 100 + H/2, 2);
        }
    }

    if (isVerifier && !sent) {
        display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        display.drawString("Is this in the dictionary?", 160, 158, 2);

        display.fillRoundRect(20, 178, 120, 44, 10, TFT_DARKGREEN);
        display.drawRoundRect(20, 178, 120, 44, 10, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
        display.drawString("YES", 80, 200, 4);

        display.fillRoundRect(180, 178, 120, 44, 10, TFT_MAROON);
        display.drawRoundRect(180, 178, 120, 44, 10, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_MAROON);
        display.drawString("NO", 240, 200, 4);
    }
}

static void renderCountdownLetResult(const CountdownWireState& state,
                                      const flip7::countdown::LettersRoundProjection& proj,
                                      bool accepted, const char* word,
                                      uint8_t wordLen, int32_t points,
                                      bool morePresentersComing) {
    display.fillScreen(TFT_BLACK);
    renderCountdownRoundHeader(state, false);
    renderLetterStrip(proj.letters, proj.letterCount);

    display.setTextDatum(MC_DATUM);
    if (accepted) {
        display.setTextColor(TFT_GREEN, TFT_BLACK);
        display.drawString("ACCEPTED!", 160, 65, 4);
        char wbuf[12]{};
        memcpy(wbuf, word, wordLen);
        char detail[32];
        snprintf(detail, sizeof(detail), "%s \u2014 %u letters", wbuf,
                 static_cast<unsigned>(wordLen));
        display.setTextColor(TFT_WHITE, TFT_BLACK);
        display.drawString(detail, 160, 115, 2);
        char ptsbuf[16];
        snprintf(ptsbuf, sizeof(ptsbuf), "+%ld POINTS",
                 static_cast<long>(points));
        display.setTextColor(TFT_YELLOW, TFT_BLACK);
        display.drawString(ptsbuf, 160, 145, 4);
    } else {
        display.setTextColor(TFT_RED, TFT_BLACK);
        display.drawString("REJECTED", 160, 90, 4);
        display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        display.drawString("0 POINTS", 160, 145, 4);
    }
    if (morePresentersComing) {
        display.setTextColor(TFT_CYAN, TFT_BLACK);
        display.drawString("Next up: opponent", 160, 198, 2);
    }
    (void)proj;
}

// ---------------------------------------------------------------------------
// Conundrum render helpers
// ---------------------------------------------------------------------------

// Compute the pixel centre of the i-th letter in the circle.
// Uses integer maths; accurate enough for a 320x240 touch screen.
static void conCirclePos(uint8_t idx, uint8_t circleOrder[9],
                          int16_t cx, int16_t cy, int16_t r,
                          int16_t& outX, int16_t& outY) {
    // Angles: 0° = top, clockwise.  i * 40°.
    int32_t deg = static_cast<int32_t>(circleOrder[idx]) * 40 - 90;
    float   rad = deg * 3.14159f / 180.0f;
    outX = static_cast<int16_t>(cx + static_cast<int16_t>(r * cosf(rad)));
    outY = static_cast<int16_t>(cy + static_cast<int16_t>(r * sinf(rad)));
}

static void renderCountdownConActive(const CountdownWireState& state,
                                      const flip7::countdown::ConundrumRoundProjection& proj,
                                      const ConUiState& con,
                                      uint32_t elapsedMs) {
    display.fillScreen(TFT_BLACK);
    renderCountdownRoundHeader(state, false);

    constexpr int16_t CX = 160, CY = 125, CR = 75;
    constexpr int16_t TW = 26, TH = 26;

    // Hint
    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    char hint[52]{};
    strncpy(hint, proj.hint, 50);
    display.drawString(hint, CX, 44, 2);

    // "Good Try" flash
    if (con.showGoodTry) {
        display.setTextColor(TFT_ORANGE, TFT_BLACK);
        display.drawString("Good try! Try again...", CX, 60, 2);
    }

    // Circular letter tiles
    for (uint8_t i = 0; i < 9; ++i) {
        int16_t tx, ty;
        conCirclePos(i, const_cast<uint8_t*>(con.circleOrder), CX, CY, CR,
                     tx, ty);
        tx = static_cast<int16_t>(tx - TW / 2);
        ty = static_cast<int16_t>(ty - TH / 2);

        // Check if this position has been selected
        bool sel = false;
        for (uint8_t s = 0; s < con.selectedCount; ++s) {
            if (con.selected[s] == i) { sel = true; break; }
        }
        uint16_t bg = sel ? TFT_DARKGREY : TFT_MAGENTA;
        display.fillRoundRect(tx, ty, TW, TH, 4, bg);
        display.drawRoundRect(tx, ty, TW, TH, 4, TFT_WHITE);
        display.setTextColor(TFT_WHITE, bg);
        // Map circle position to scramble letter via circleOrder
        char s[2] = {proj.scramble[con.circleOrder[i]], '\0'};
        display.drawString(s, tx + TW/2, ty + TH/2, 2);
    }

    // Reshuffle icon (top-right, outside header)
    display.fillRoundRect(284, 38, 30, 22, 4, TFT_DARKGREY);
    display.drawRoundRect(284, 38, 30, 22, 4, TFT_WHITE);
    display.setTextColor(TFT_WHITE, TFT_DARKGREY);
    display.drawString("~>", 299, 49, 2);

    // Answer strip
    for (uint8_t i = 0; i < 9; ++i) {
        int16_t bx = static_cast<int16_t>(6 + i * 34);
        uint16_t bg = (i < con.selectedCount) ? TFT_DARKCYAN : TFT_DARKGREY;
        display.fillRect(bx, 205, 30, 28, bg);
        display.drawRect(bx, 205, 30, 28, TFT_WHITE);
        if (i < con.selectedCount) {
            display.setTextDatum(MC_DATUM);
            display.setTextColor(TFT_WHITE, bg);
            char ch[2] = {proj.scramble[con.circleOrder[con.selected[i]]], '\0'};
            display.drawString(ch, bx + 15, 219, 2);
        }
    }

    // Timer (top-right corner of circle area)
    renderCountdownAnimation(kCdTimerCX, kCdTimerCY, kCdTimerR,
                             elapsedMs, 30000);
}

static void renderCountdownConResult(bool won, const char* solution) {
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(MC_DATUM);
    if (won) {
        display.setTextColor(TFT_GREEN, TFT_BLACK);
        display.drawString("CONUNDRUM SOLVED!", 160, 60, 4);
        display.setTextColor(TFT_WHITE, TFT_BLACK);
        display.drawString(solution, 160, 120, 4);
        display.setTextColor(TFT_YELLOW, TFT_BLACK);
        display.drawString("YOU WIN! +10 POINTS", 160, 175, 4);
    } else {
        display.setTextColor(TFT_CYAN, TFT_BLACK);
        display.drawString("CONUNDRUM SOLVED", 160, 60, 4);
        display.setTextColor(TFT_WHITE, TFT_BLACK);
        display.drawString(solution, 160, 120, 4);
        display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        display.drawString("Good try!", 160, 175, 2);
    }
}

static void renderCountdownConNoWinner(const char* solution) {
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_RED, TFT_BLACK);
    display.drawString("TIME'S UP", 160, 60, 4);
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.drawString("The answer was:", 160, 120, 2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(solution, 160, 155, 4);
    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    display.drawString("No points awarded", 160, 205, 2);
}

// ---------------------------------------------------------------------------
// Letters + Conundrum sub-phase dispatcher
// ---------------------------------------------------------------------------
static void renderCountdownLetConPhases(  // NOLINT
    bool online, const CountdownWireState& state,
    CountdownRoundSubPhase subPhase, uint32_t elapsed) {
    using SubPhase = CountdownRoundSubPhase;

    // Letters sub-phases
    if (subPhase == SubPhase::LetPicking) {
        flip7::countdown::LettersRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.lettersProjection();
        portEXIT_CRITICAL(&gameMux);
        renderCountdownLetPicking(state, proj, letUi.showConsonants);
        return;
    }
    if (subPhase == SubPhase::LetThinking) {
        flip7::countdown::LettersRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.lettersProjection();
        portEXIT_CRITICAL(&gameMux);
        renderCountdownLetThinking(state, proj, elapsed,
                                   lastAnimFrameMs < roundPhaseStartMs);
        return;
    }
    if (subPhase == SubPhase::LetClaimEntry) {
        flip7::countdown::LettersRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.lettersProjection();
        portEXIT_CRITICAL(&gameMux);
        renderCountdownLetClaimEntry(state, proj,
                                      letUi.claimLength, letUi.claimSent);
        return;
    }
    if (subPhase == SubPhase::LetClaimReveal) {
        flip7::countdown::LettersRoundProjection proj{};
        uint8_t hClaim = 0, gClaim = 0;
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.lettersProjection();
        auto* lr = countdownEngine.lettersRound();
        if (lr) {
            hClaim = lr->claimFor(state.hostBoardId);
            gClaim = lr->claimFor(state.guestBoardId);
        }
        portEXIT_CRITICAL(&gameMux);
        renderCountdownLetClaimReveal(state, proj, hClaim, gClaim);
        return;
    }
    if (subPhase == SubPhase::LetPresentPlayerA ||
        subPhase == SubPhase::LetPresentPlayerB) {
        flip7::countdown::LettersRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.lettersProjection();
        portEXIT_CRITICAL(&gameMux);
        bool presenterIsHost = letUi.presenterIsHost;
        bool aPhase = (subPhase == SubPhase::LetPresentPlayerA);
        bool isPresenter =
            (aPhase && ((presenterIsHost && boardId == state.hostBoardId) ||
                        (!presenterIsHost && boardId == state.guestBoardId))) ||
            (!aPhase && ((presenterIsHost && boardId == state.guestBoardId) ||
                         (!presenterIsHost && boardId == state.hostBoardId)));
        bool inVerifyMode = letUi.presentationSent;
        if (inVerifyMode) {
            // Show verification UI to the other board; waiting for presenter
            bool isVerifier = !isPresenter;
            portENTER_CRITICAL(&gameMux);
            auto* lr = countdownEngine.lettersRound();
            uint32_t presenterBoardId =
                (aPhase && presenterIsHost) ? state.hostBoardId
                                             : state.guestBoardId;
            const std::string& pw =
                lr ? lr->presentedWordFor(presenterBoardId) : std::string{};
            portEXIT_CRITICAL(&gameMux);
            renderCountdownLetVerification(state, pw.c_str(),
                                            static_cast<uint8_t>(pw.size()),
                                            isVerifier, letUi.verificationSent);
        } else {
            renderCountdownLetPresenting(state, proj, isPresenter,
                                          letUi.word, letUi.wordLen,
                                          letUi.presentationSent);
        }
        return;
    }
    if (subPhase == SubPhase::LetResult) {
        flip7::countdown::LettersRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.lettersProjection();
        portEXIT_CRITICAL(&gameMux);
        // Show accepted/points from last verification result
        bool accepted = letUi.verificationAnswer;
        int32_t points = accepted
            ? (letUi.claimLength == 9 ? 18 : letUi.claimLength)
            : 0;
        renderCountdownLetResult(state, proj, accepted,
                                  letUi.word, letUi.wordLen,
                                  points, false);
        return;
    }

    // Conundrum sub-phases
    if (subPhase == SubPhase::ConReady) {
        renderCountdownIntro(state, elapsed,
                             lastAnimFrameMs < roundPhaseStartMs);  // reuse intro screen
        return;
    }
    if (subPhase == SubPhase::ConActive) {
        // "Good Try" auto-clear after 1.5 seconds
        if (conUi.showGoodTry && millis() - conUi.goodTryMs > 1500) {
            conUi.showGoodTry = false;
        }
        flip7::countdown::ConundrumRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.conundrumProjection();
        portEXIT_CRITICAL(&gameMux);
        renderCountdownConActive(state, proj, conUi, elapsed);
        return;
    }
    if (subPhase == SubPhase::ConResult) {
        flip7::countdown::ConundrumRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.conundrumProjection();
        portEXIT_CRITICAL(&gameMux);
        bool won = (proj.winnerBoardId == boardId);
        renderCountdownConResult(won, proj.scramble);  // show solution
        return;
    }
    if (subPhase == SubPhase::ConResultNoWinner) {
        flip7::countdown::ConundrumRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.conundrumProjection();
        portEXIT_CRITICAL(&gameMux);
        renderCountdownConNoWinner(proj.scramble);
        return;
    }
    if (subPhase == SubPhase::HostTransfer ||
        subPhase == SubPhase::HostTransferAck) {
        display.fillScreen(TFT_BLACK);
        renderCountdownRoundHeader(state, false);
        display.setTextDatum(MC_DATUM);
        bool isOldHost = (boardId != state.hostBoardId);
        display.setTextColor(TFT_YELLOW, TFT_BLACK);
        display.drawString(isOldHost ? "TRANSFERRING HOST..."
                                     : "ACKNOWLEDGED",
                           160, 90, 4);
        display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        display.drawString(isOldHost ? "Sending game state..."
                                     : "State received. You are now the Host.",
                           160, 150, 2);
        if (subPhase == SubPhase::HostTransferAck) {
            display.fillCircle(160, 195, 14, TFT_DARKGREEN);
            display.drawCircle(160, 195, 14, TFT_WHITE);
            display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
            display.drawString("OK", 160, 195, 2);
        }
        return;
    }

    // Generic fallback for any remaining sub-phase
    display.fillScreen(TFT_BLACK);
    renderCountdownRoundHeader(state, online);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_YELLOW, TFT_BLACK);
    display.drawString("IN ROUND", 160, 100, 4);
    char spLabel[32];
    snprintf(spLabel, sizeof(spLabel), "Sub-phase %u", state.roundSubPhase);
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.drawString(spLabel, 160, 145, 2);
}

void renderScreen() {
    PuzzleState game{};
    MastermindState mastermind{};
    CountdownWireState countdown{};
    ScreenMode mode;
    bool ready;
    bool mastermindReady;
    bool countdownReady;
    bool deliveryPending;
    bool mastermindDeliveryPending;
    portENTER_CRITICAL(&gameMux);
    game = puzzleState;
    mastermind = mastermindState;
    countdown = countdownState;
    mode = screenMode;
    ready = stateReady;
    mastermindReady = mastermindStateReady;
    countdownReady = countdownStateReady;
    deliveryPending = pendingDelivery.active;
    mastermindDeliveryPending = mastermindPendingDelivery.active;
    portEXIT_CRITICAL(&gameMux);

    const bool online = peerOnline();
    if (mode == ScreenMode::Home) {
        renderHome(online, deliveryPending);
    } else if (mode == ScreenMode::Complete) {
        renderComplete(online, game);
    } else if (mode == ScreenMode::Mastermind && mastermindReady) {
        renderMastermind(online, mastermind, mastermindDeliveryPending);
    } else if (mode == ScreenMode::Countdown && countdownReady) {
        renderCountdown(online, countdown);
    } else if (mode == ScreenMode::Puzzle && ready) {
        renderPuzzle(online, game, deliveryPending);
    } else {
        renderHome(online, deliveryPending);
    }
}

void queueAck(uint32_t targetBoardId, const PuzzleState& state,
              MessageType acknowledgedType) {
    pendingAck = PendingAck{true,
                            targetBoardId,
                            state.gameId,
                            state.revision,
                            puzzleStateDigest(state),
                            acknowledgedType};
}

bool validMastermindIdentity(const MastermindState& state) {
    const uint32_t host = std::min(boardId, gamePeerBoardId);
    const uint32_t guest = std::max(boardId, gamePeerBoardId);
    return state.hostBoardId == host && state.guestBoardId == guest;
}

void prepareMastermindDraft(const MastermindState& state) {
    if (state.phase == MastermindPhase::Guessing &&
        state.codemakerBoardId != boardId) {
        draftCode = lastSubmittedGuess;
    } else {
        draftCode = MastermindCode{{1, 1, 1, 1}};
    }
}

bool validCountdownIdentity(const CountdownWireState& state) {
    const uint32_t host = std::min(boardId, gamePeerBoardId);
    const uint32_t guest = std::max(boardId, gamePeerBoardId);
    return state.hostBoardId == host && state.guestBoardId == guest;
}

void queueCountdownAck(uint32_t targetBoardId,
                       const CountdownWireState& state,
                       MessageType acknowledgedType) {
    countdownPendingAck = CountdownPendingAck{
        true, targetBoardId, state.gameId, state.revision,
        countdownStateDigest(state), acknowledgedType};
}

void prepareMastermindDraft() {
    draftCode = MastermindCode{{1, 1, 1, 1}};
    selectedPeg = 0;
}

void noteMastermindPhase(const MastermindState& state) {
    prepareMastermindDraft(state);
    if (state.phase == MastermindPhase::RoundComplete) {
        mastermindCompletedAtMs = millis();
    }
}

void queueMastermindAck(uint32_t targetBoardId,
                        const MastermindState& state,
                        MessageType acknowledgedType) {
    mastermindPendingAck = MastermindPendingAck{
        true, targetBoardId, state.gameId, state.revision,
        mastermindStateDigest(state), acknowledgedType};
}

void onPacketSent(const uint8_t*, esp_now_send_status_t status) {
    portENTER_CRITICAL(&linkMux);
    if (status == ESP_NOW_SEND_SUCCESS) {
        ++linkState.sent;
    } else {
        ++linkState.sendFailures;
    }
    portEXIT_CRITICAL(&linkMux);
}

void processStatePacket(const uint8_t* address, const uint8_t* data,
                        int length, MessageType type) {
    if (length != sizeof(PuzzleStatePacket)) {
        return;
    }
    PuzzleStatePacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (!validHeader(packet.header, type) ||
        !matchesBoundPeer(address, packet.header.senderId) ||
        !isValidPuzzle(packet.state) ||
        !acceptPeerSequence(type, packet.header.sessionId,
                            packet.header.sequence)) {
        return;
    }

    bool accepted = false;
    bool duplicate = false;
    portENTER_CRITICAL(&gameMux);
    if (!shouldAcceptGameState(activeGame, ActiveGameKind::Puzzle,
                               packet.state.gameId)) {
        portEXIT_CRITICAL(&gameMux);
        return;
    }
    const bool validTurnId = packet.state.turnBoardId == boardId ||
                             packet.state.turnBoardId == gamePeerBoardId;
    const bool exitSignalApplied =
        validTurnId && stateReady &&
        packet.state.phase == PuzzlePhase::Exited &&
        applyPuzzleExitSignal(puzzleState, packet.state,
                              packet.header.senderId, boardId);
    if (exitSignalApplied) {
        const bool terminalConverged =
            isSamePuzzle(puzzleState, packet.state);
        pendingDelivery.active = false;
        reconciliationPending = false;
        sendFullStateSoon = !terminalConverged;
        screenMode = ScreenMode::Home;
        completedAtMs = 0;
        displayDirty = true;
        accepted = true;
    } else if (validTurnId && type == MessageType::FullState) {
        duplicate = stateReady && isSamePuzzle(puzzleState, packet.state);
        const PuzzleVersionOrder order =
            stateReady ? comparePuzzleVersion(puzzleState, packet.state)
                       : PuzzleVersionOrder::Newer;
        const bool equalConflict =
            stateReady && order == PuzzleVersionOrder::Same && !duplicate;
        const bool peerIsAuthority = packet.header.senderId < boardId;
        const bool participantsValid = isValidPuzzleForParticipants(
            packet.state, packet.header.senderId, boardId);
        const bool adopt =
            !stateReady
                ? participantsValid
                : shouldAdoptFullState(puzzleState, packet.state,
                                       packet.header.senderId, boardId) ||
                      (equalConflict && peerIsAuthority && participantsValid);
        if (adopt) {
            puzzleState = packet.state;
            stateReady = true;
            pendingDelivery.active = false;
            reconciliationPending = false;
            if (puzzleState.phase == PuzzlePhase::Exited) {
                screenMode = ScreenMode::Home;
                completedAtMs = 0;
            } else if (isPuzzleSolved(puzzleState)) {
                screenMode = ScreenMode::Complete;
                completedAtMs = millis();
            } else {
                screenMode = ScreenMode::Puzzle;
            }
            displayDirty = true;
            fullLogGameId = puzzleState.gameId;
            fullLogRevision = puzzleState.revision;
            fullLogPending = true;
            accepted = true;
        } else if (!duplicate && stateReady) {
            if (boardId < packet.header.senderId) {
                sendFullStateSoon = true;
            } else {
                requestStateSoon = true;
                reconciliationPending = true;
            }
        }
    } else if (validTurnId && type == MessageType::PuzzleState) {
        duplicate = stateReady && isSamePuzzle(puzzleState, packet.state);
        if (!stateReady && packet.state.revision == 1) {
            PuzzleState base = makeScrambledPuzzle(
                std::min(boardId, gamePeerBoardId), packet.state.gameId,
                puzzleSpec(packet.state));
            if (isValidRemoteTransition(base, packet.state,
                                        packet.header.senderId, boardId)) {
                puzzleState = packet.state;
                stateReady = true;
                accepted = true;
            }
        } else if (stateReady &&
                   isValidRemoteTransition(puzzleState, packet.state,
                                           packet.header.senderId, boardId)) {
            puzzleState = packet.state;
            accepted = true;
        }
        if (accepted) {
            if (pendingDelivery.active &&
                isDeliverySuperseded(pendingDelivery.state, puzzleState)) {
                pendingDelivery.active = false;
            }
            reconciliationPending = false;
            screenMode = isPuzzleSolved(puzzleState) ? ScreenMode::Complete
                                                      : ScreenMode::Puzzle;
            if (screenMode == ScreenMode::Complete) {
                completedAtMs = millis();
            }
            displayDirty = true;
            remoteLogRevision = puzzleState.revision;
            remoteLogSender = packet.header.senderId;
            remoteLogPending = true;
        } else if (!duplicate) {
            requestStateSoon = true;
            reconciliationPending = true;
        }
    }
    if (accepted) {
        activeGame = {
            puzzleState.gameId,
            puzzleState.phase == PuzzlePhase::Exited
                ? ActiveGameKind::Home
                : ActiveGameKind::Puzzle,
        };
        mastermindPendingDelivery.active = false;
        mastermindReconciliationPending = false;
        mastermindRequestStateSoon = false;
        sendFullStateSoon = false;
        sendMastermindFullStateSoon = false;
    }
    if (duplicate && pendingDelivery.active &&
        isDeliverySuperseded(pendingDelivery.state, packet.state)) {
        pendingDelivery.active = false;
        displayDirty = true;
    }
    if (accepted || duplicate) {
        queueAck(packet.header.senderId, packet.state, type);
    }
    portEXIT_CRITICAL(&gameMux);
}

void processAckPacket(const uint8_t* address, const uint8_t* data,
                      int length) {
    if (length != sizeof(PuzzleAckPacket)) {
        return;
    }
    PuzzleAckPacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (!validHeader(packet.header, MessageType::PuzzleAck) ||
        packet.targetBoardId != boardId ||
        !matchesBoundPeer(address, packet.header.senderId) ||
        !acceptPeerSequence(MessageType::PuzzleAck,
                            packet.header.sessionId,
                            packet.header.sequence)) {
        return;
    }
    portENTER_CRITICAL(&gameMux);
    if (pendingDelivery.active &&
        pendingDelivery.type == packet.acknowledgedType &&
        pendingDelivery.state.gameId == packet.gameId &&
        pendingDelivery.state.revision == packet.revision &&
        puzzleStateDigest(pendingDelivery.state) == packet.stateDigest) {
        pendingDelivery.active = false;
        displayDirty = true;
        ackLogGameId = packet.gameId;
        ackLogRevision = packet.revision;
        ackLogPending = true;
    }
    portEXIT_CRITICAL(&gameMux);
}

void processStateRequest(const uint8_t* address, const uint8_t* data,
                         int length) {
    if (length != sizeof(StateRequestPacket)) {
        return;
    }
    StateRequestPacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (!validHeader(packet.header, MessageType::RequestState) ||
        !matchesBoundPeer(address, packet.header.senderId) ||
        !acceptPeerSequence(MessageType::RequestState,
                            packet.header.sessionId,
                            packet.header.sequence)) {
        return;
    }
    portENTER_CRITICAL(&gameMux);
    if (stateReady && (screenMode == ScreenMode::Puzzle ||
                       screenMode == ScreenMode::Complete)) {
        const ReconciliationAction action = decideReconciliation(
            puzzleState, packet.gameId, packet.revision, packet.stateDigest,
            boardId < packet.header.senderId);
        if (action == ReconciliationAction::SendFullState) {
            sendFullStateSoon = true;
        } else if (action == ReconciliationAction::RequestFullState) {
            requestStateSoon = true;
            reconciliationPending = true;
        } else {
            reconciliationPending = false;
        }
    }
    portEXIT_CRITICAL(&gameMux);
}

void processMastermindStatePacket(const uint8_t* address,
                                  const uint8_t* data, int length,
                                  MessageType type) {
    if (length != sizeof(GameStatePacket)) {
        return;
    }
    GameStatePacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (!validHeader(packet.header, type) ||
        !matchesBoundPeer(address, packet.header.senderId) ||
        !isValidMastermindState(packet.state) ||
        !validMastermindIdentity(packet.state) ||
        !acceptPeerSequence(type, packet.header.sessionId,
                            packet.header.sequence)) {
        return;
    }

    bool accepted = false;
    bool duplicate = false;
    portENTER_CRITICAL(&gameMux);
    const bool terminal = packet.state.phase == MastermindPhase::Exited;
    if (!shouldAcceptGameState(activeGame, ActiveGameKind::Mastermind,
                               packet.state.gameId, terminal)) {
        portEXIT_CRITICAL(&gameMux);
        return;
    }
    duplicate = mastermindStateReady &&
                sameMastermindState(mastermindState, packet.state);
    const bool exitSignalApplied =
        type == MessageType::MastermindState && terminal &&
        mastermindStateReady &&
        applyMastermindExitSignal(mastermindState, packet.state,
                                  packet.header.senderId);
    if (exitSignalApplied) {
        mastermindPendingDelivery.active = false;
        mastermindReconciliationPending = false;
        accepted = true;
    } else {
        const MastermindVersionOrder order =
            mastermindStateReady
                ? compareMastermindVersion(mastermindState, packet.state)
                : MastermindVersionOrder::Newer;
        if (type == MessageType::MastermindFullState) {
            const bool equalConflict = mastermindStateReady &&
                order == MastermindVersionOrder::Same && !duplicate;
            const bool peerIsAuthority = packet.header.senderId < boardId;
            if (!mastermindStateReady ||
                order == MastermindVersionOrder::Newer ||
                (equalConflict && peerIsAuthority)) {
                mastermindState = packet.state;
                mastermindStateReady = true;
                mastermindPendingDelivery.active = false;
                mastermindReconciliationPending = false;
                accepted = true;
            } else if (order == MastermindVersionOrder::Older ||
                       (equalConflict && !peerIsAuthority)) {
                sendMastermindFullStateSoon = true;
            }
        } else if (type == MessageType::MastermindState) {
            if (!mastermindStateReady ||
                isValidMastermindTransition(mastermindState, packet.state,
                                            packet.header.senderId)) {
                mastermindState = packet.state;
                mastermindStateReady = true;
                accepted = true;
            } else if (!duplicate) {
                mastermindRequestStateSoon = true;
                mastermindReconciliationPending = true;
            }
        }
    }
    if ((accepted || duplicate) && mastermindPendingDelivery.active &&
        isMastermindDeliverySuperseded(mastermindPendingDelivery.state,
                                      packet.state)) {
        mastermindPendingDelivery.active = false;
    }
    if (accepted) {
        pendingDelivery.active = false;
        reconciliationPending = false;
        requestStateSoon = false;
        sendFullStateSoon = false;
        sendMastermindFullStateSoon = false;
        activeGame = {
            mastermindState.gameId,
            mastermindState.phase == MastermindPhase::Exited
                ? ActiveGameKind::Home
                : ActiveGameKind::Mastermind,
        };
        screenMode = mastermindState.phase == MastermindPhase::Exited
                         ? ScreenMode::Home
                         : ScreenMode::Mastermind;
        mastermindReconciliationPending = false;
        noteMastermindPhase(mastermindState);
        displayDirty = true;
    }
    if (accepted || duplicate) {
        queueMastermindAck(packet.header.senderId, packet.state, type);
    }
    portEXIT_CRITICAL(&gameMux);
}

void processMastermindAckPacket(const uint8_t* address,
                                const uint8_t* data, int length) {
    if (length != sizeof(GameAckPacket)) {
        return;
    }
    GameAckPacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (!validHeader(packet.header, MessageType::MastermindAck) ||
        packet.targetBoardId != boardId ||
        !matchesBoundPeer(address, packet.header.senderId) ||
        !acceptPeerSequence(MessageType::MastermindAck,
                            packet.header.sessionId,
                            packet.header.sequence)) {
        return;
    }
    portENTER_CRITICAL(&gameMux);
    if (mastermindPendingDelivery.active &&
        mastermindPendingDelivery.type == packet.acknowledgedType &&
        mastermindPendingDelivery.state.gameId == packet.gameId &&
        mastermindPendingDelivery.state.revision == packet.revision &&
        mastermindStateDigest(mastermindPendingDelivery.state) ==
            packet.stateDigest) {
        mastermindPendingDelivery.active = false;
        displayDirty = true;
    }
    portEXIT_CRITICAL(&gameMux);
}

void processMastermindStateRequest(const uint8_t* address,
                                   const uint8_t* data, int length) {
    if (length != sizeof(MastermindStateRequestPacket)) {
        return;
    }
    MastermindStateRequestPacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (!validHeader(packet.header, MessageType::MastermindRequestState) ||
        !matchesBoundPeer(address, packet.header.senderId) ||
        !acceptPeerSequence(MessageType::MastermindRequestState,
                            packet.header.sessionId,
                            packet.header.sequence)) {
        return;
    }
    portENTER_CRITICAL(&gameMux);
    if (mastermindStateReady && screenMode == ScreenMode::Mastermind) {
        const bool remoteOlder = packet.gameId < mastermindState.gameId ||
            (packet.gameId == mastermindState.gameId &&
             packet.revision < mastermindState.revision);
        const bool remoteNewer = packet.gameId > mastermindState.gameId ||
            (packet.gameId == mastermindState.gameId &&
             packet.revision > mastermindState.revision);
        const bool divergent = packet.gameId == mastermindState.gameId &&
            packet.revision == mastermindState.revision &&
            packet.stateDigest != mastermindStateDigest(mastermindState);
        if (remoteOlder || (divergent && localIsHost())) {
            sendMastermindFullStateSoon = true;
        } else if (remoteNewer || (divergent && !localIsHost())) {
            mastermindRequestStateSoon = true;
            mastermindReconciliationPending = true;
        } else {
            mastermindReconciliationPending = false;
        }
    }
    portEXIT_CRITICAL(&gameMux);
}

void processCountdownStatePacket(const uint8_t* address,
                                 const uint8_t* data, int length,
                                 MessageType type) {
    if (length != sizeof(CountdownStatePacket)) {
        return;
    }
    CountdownStatePacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (!validHeader(packet.header, type) ||
        !matchesBoundPeer(address, packet.header.senderId) ||
        !isValidCountdownWireState(packet.state) ||
        !validCountdownIdentity(packet.state) ||
        !acceptPeerSequence(type, packet.header.sessionId,
                            packet.header.sequence)) {
        return;
    }

    bool accepted = false;
    bool duplicate = false;
    portENTER_CRITICAL(&gameMux);
    const bool terminal = packet.state.phase == CountdownWirePhase::Exited;
    if (!shouldAcceptGameState(activeGame, ActiveGameKind::Countdown,
                               packet.state.gameId, terminal)) {
        portEXIT_CRITICAL(&gameMux);
        return;
    }
    duplicate = countdownStateReady &&
                sameCountdownWireState(countdownState, packet.state);
    const bool exitSignalApplied =
        type == MessageType::CountdownState && terminal &&
        countdownStateReady &&
        applyCountdownExitSignal(countdownState, packet.state,
                                 packet.header.senderId);
    if (exitSignalApplied) {
        countdownPendingDelivery.active = false;
        countdownReconciliationPending = false;
        accepted = true;
    } else {
        const CountdownVersionOrder order =
            countdownStateReady
                ? compareCountdownVersion(countdownState, packet.state)
                : CountdownVersionOrder::Newer;
        if (type == MessageType::CountdownFullState) {
            const bool equalConflict = countdownStateReady &&
                order == CountdownVersionOrder::Same && !duplicate;
            const bool peerIsAuthority = packet.header.senderId < boardId;
            if (!countdownStateReady ||
                order == CountdownVersionOrder::Newer ||
                (equalConflict && peerIsAuthority)) {
                countdownState = packet.state;
                countdownStateReady = true;
                countdownPendingDelivery.active = false;
                countdownReconciliationPending = false;
                accepted = true;
            } else if (order == CountdownVersionOrder::Older ||
                       (equalConflict && !peerIsAuthority)) {
                sendCountdownFullStateSoon = true;
            }
        } else if (type == MessageType::CountdownState) {
            if (!countdownStateReady ||
                (packet.state.gameId == countdownState.gameId &&
                 packet.state.revision == countdownState.revision + 1)) {
                countdownState = packet.state;
                countdownStateReady = true;
                accepted = true;
            } else if (!duplicate) {
                countdownRequestStateSoon = true;
                countdownReconciliationPending = true;
            }
        }
    }
    if ((accepted || duplicate) && countdownPendingDelivery.active &&
        isCountdownDeliverySuperseded(countdownPendingDelivery.state,
                                      packet.state)) {
        countdownPendingDelivery.active = false;
    }
    if (accepted) {
        pendingDelivery.active = false;
        reconciliationPending = false;
        requestStateSoon = false;
        sendFullStateSoon = false;
        sendMastermindFullStateSoon = false;
        sendCountdownFullStateSoon = false;
        activeGame = {
            countdownState.gameId,
            countdownState.phase == CountdownWirePhase::Exited
                ? ActiveGameKind::Home
                : ActiveGameKind::Countdown,
        };
        screenMode = countdownState.phase == CountdownWirePhase::Exited
                         ? ScreenMode::Home
                         : ScreenMode::Countdown;
        countdownReconciliationPending = false;

        // Sync the local engine round so projections (target, tiles, letters,
        // conundrum) are available for rendering — even on the guest board.
        // The host already called startNextRound directly; skip if already done.
        if (countdownState.phase == CountdownWirePhase::InRound &&
            boardId != countdownState.hostBoardId) {
            bool needsRoundStart =
                countdownEngine.activeRound() == nullptr ||
                countdownEngine.roundNumber() != countdownState.roundNumber;
            if (needsRoundStart) {
                auto rt = static_cast<flip7::countdown::RoundType>(
                    countdownState.roundType);
                auto rng = flip7::countdown::makeRoundRandom(
                    countdownState.gameId, countdownState.roundNumber);
                uint8_t largeCnt = countdownState.roundConfig;
                countdownEngine.startNextRound(rt, rng, largeCnt);
            }
        }

        displayDirty = true;
        lastAnimFrameMs = 0;  // force first-frame full redraw on new state
    }
    if (accepted || duplicate) {
        queueCountdownAck(packet.header.senderId, packet.state, type);
    }
    portEXIT_CRITICAL(&gameMux);
}

void processCountdownAckPacket(const uint8_t* address,
                               const uint8_t* data, int length) {
    if (length != sizeof(CountdownAckPacket)) {
        return;
    }
    CountdownAckPacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (!validHeader(packet.header, MessageType::CountdownAck) ||
        packet.targetBoardId != boardId ||
        !matchesBoundPeer(address, packet.header.senderId) ||
        !acceptPeerSequence(MessageType::CountdownAck,
                            packet.header.sessionId,
                            packet.header.sequence)) {
        return;
    }
    portENTER_CRITICAL(&gameMux);
    if (countdownPendingDelivery.active &&
        countdownPendingDelivery.type == packet.acknowledgedType &&
        countdownPendingDelivery.state.gameId == packet.gameId &&
        countdownPendingDelivery.state.revision == packet.revision &&
        countdownStateDigest(countdownPendingDelivery.state) ==
            packet.stateDigest) {
        countdownPendingDelivery.active = false;
        displayDirty = true;
    }
    portEXIT_CRITICAL(&gameMux);
}

void processCountdownStateRequest(const uint8_t* address,
                                  const uint8_t* data, int length) {
    if (length != sizeof(CountdownStateRequestPacket)) {
        return;
    }
    CountdownStateRequestPacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (!validHeader(packet.header, MessageType::CountdownRequestState) ||
        !matchesBoundPeer(address, packet.header.senderId) ||
        !acceptPeerSequence(MessageType::CountdownRequestState,
                            packet.header.sessionId,
                            packet.header.sequence)) {
        return;
    }
    portENTER_CRITICAL(&gameMux);
    if (countdownStateReady && screenMode == ScreenMode::Countdown) {
        const bool remoteOlder = packet.gameId < countdownState.gameId ||
            (packet.gameId == countdownState.gameId &&
             packet.revision < countdownState.revision);
        const bool remoteNewer = packet.gameId > countdownState.gameId ||
            (packet.gameId == countdownState.gameId &&
             packet.revision > countdownState.revision);
        const bool divergent = packet.gameId == countdownState.gameId &&
            packet.revision == countdownState.revision &&
            packet.stateDigest != countdownStateDigest(countdownState);
        if (remoteOlder || (divergent && localIsHost())) {
            sendCountdownFullStateSoon = true;
        } else if (remoteNewer || (divergent && !localIsHost())) {
            countdownRequestStateSoon = true;
            countdownReconciliationPending = true;
        } else {
            countdownReconciliationPending = false;
        }
    }
    portEXIT_CRITICAL(&gameMux);
}

void onPacketReceived(const uint8_t* address, const uint8_t* data, int length) {
    if (length < sizeof(PacketHeader)) {
        return;
    }
    PacketHeader header{};
    memcpy(&header, data, sizeof(header));
    if (header.magic != kProtocolMagic || header.version != kProtocolVersion) {
        return;
    }

    if (header.type == MessageType::Heartbeat &&
        length == sizeof(HeartbeatPacket) &&
        validHeader(header, MessageType::Heartbeat)) {
        bindOrMatchPeer(address, header.senderId, header.sessionId,
                        header.sequence);
        return;
    }
    if (protocolMutex == nullptr ||
        xSemaphoreTake(protocolMutex, 0) != pdTRUE) {
        return;
    }
    if (header.type == MessageType::PuzzleState ||
        header.type == MessageType::FullState) {
        processStatePacket(address, data, length, header.type);
    } else if (header.type == MessageType::PuzzleAck) {
        processAckPacket(address, data, length);
    } else if (header.type == MessageType::RequestState) {
        processStateRequest(address, data, length);
    } else if (header.type == MessageType::MastermindState ||
               header.type == MessageType::MastermindFullState) {
        processMastermindStatePacket(address, data, length, header.type);
    } else if (header.type == MessageType::MastermindAck) {
        processMastermindAckPacket(address, data, length);
    } else if (header.type == MessageType::MastermindRequestState) {
        processMastermindStateRequest(address, data, length);
    } else if (header.type == MessageType::CountdownState ||
               header.type == MessageType::CountdownFullState) {
        processCountdownStatePacket(address, data, length, header.type);
    } else if (header.type == MessageType::CountdownAck) {
        processCountdownAckPacket(address, data, length);
    } else if (header.type == MessageType::CountdownRequestState) {
        processCountdownStateRequest(address, data, length);
    }
    xSemaphoreGive(protocolMutex);
}

bool startEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if (!configureExpectedPeer() || esp_now_init() != ESP_OK) {
        return false;
    }
    esp_now_register_send_cb(onPacketSent);
    esp_now_register_recv_cb(onPacketReceived);
    if (esp_now_set_pmk(kEspNowPmk) != ESP_OK) {
        esp_now_deinit();
        return false;
    }

    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, expectedPeerAddress, sizeof(expectedPeerAddress));
    memcpy(peer.lmk, kEspNowLmk, sizeof(peer.lmk));
    peer.channel = 0;
    peer.encrypt = true;
    if (esp_now_add_peer(&peer) == ESP_OK) {
        return true;
    }
    esp_now_deinit();
    return false;
}

void sendHeartbeat() {
    HeartbeatPacket packet{makeHeader(MessageType::Heartbeat), millis()};
    esp_now_send(expectedPeerAddress, reinterpret_cast<uint8_t*>(&packet),
                 sizeof(packet));
}

void sendAck(const PendingAck& ack) {
    PuzzleAckPacket packet{makeHeader(MessageType::PuzzleAck),
                           ack.targetBoardId,
                           ack.gameId,
                           ack.revision,
                           ack.stateDigest,
                           ack.acknowledgedType,
                           {0, 0, 0}};
    esp_now_send(expectedPeerAddress, reinterpret_cast<uint8_t*>(&packet),
                 sizeof(packet));
}

void sendStateRequest() {
    uint32_t gameId = 0;
    uint32_t revision = 0;
    uint32_t stateDigest = 0;
    portENTER_CRITICAL(&gameMux);
    if (stateReady) {
        gameId = puzzleState.gameId;
        revision = puzzleState.revision;
        stateDigest = puzzleStateDigest(puzzleState);
    }
    portEXIT_CRITICAL(&gameMux);
    StateRequestPacket packet{makeHeader(MessageType::RequestState), gameId,
                              revision, stateDigest};
    esp_now_send(expectedPeerAddress, reinterpret_cast<uint8_t*>(&packet),
                 sizeof(packet));
}

void sendPuzzleStateIfCurrent(MessageType type, const PuzzleState& state) {
    PuzzleStatePacket packet{makeHeader(type), state};
    if (protocolMutex == nullptr ||
        xSemaphoreTake(protocolMutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&gameMux);
    const bool snapshotMatches =
        stateReady && memcmp(&puzzleState, &state, sizeof(state)) == 0;
    const bool shouldSend =
        shouldSendActiveGameState(activeGame, ActiveGameKind::Puzzle,
                                  state.gameId, snapshotMatches);
    portEXIT_CRITICAL(&gameMux);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (shouldSend) {
        result = esp_now_send(expectedPeerAddress,
                              reinterpret_cast<uint8_t*>(&packet),
                              sizeof(packet));
    }
    xSemaphoreGive(protocolMutex);
    if (shouldSend) {
        Serial.printf("TX %s game=%lu revision=%lu result=%d\n",
                      type == MessageType::FullState ? "full" : "move",
                      static_cast<unsigned long>(state.gameId),
                      static_cast<unsigned long>(state.revision), result);
    }
}

void sendMastermindStateIfCurrent(MessageType type,
                                  const MastermindState& state) {
    GameStatePacket packet{makeHeader(type), state};
    if (protocolMutex == nullptr ||
        xSemaphoreTake(protocolMutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&gameMux);
    const bool snapshotMatches =
        mastermindStateReady && sameMastermindState(mastermindState, state);
    const bool terminalDelivery =
        type == MessageType::MastermindState &&
        state.phase == MastermindPhase::Exited;
    const bool shouldSend =
        shouldSendActiveGameState(activeGame, ActiveGameKind::Mastermind,
                                  state.gameId, snapshotMatches,
                                  terminalDelivery);
    portEXIT_CRITICAL(&gameMux);
    if (shouldSend) {
        esp_now_send(expectedPeerAddress, reinterpret_cast<uint8_t*>(&packet),
                     sizeof(packet));
    }
    xSemaphoreGive(protocolMutex);
}

void sendMastermindAck(const MastermindPendingAck& ack) {
    GameAckPacket packet{makeHeader(MessageType::MastermindAck),
                         ack.targetBoardId,
                         ack.gameId,
                         ack.revision,
                         ack.stateDigest,
                         ack.acknowledgedType,
                         {0, 0, 0}};
    esp_now_send(expectedPeerAddress, reinterpret_cast<uint8_t*>(&packet),
                 sizeof(packet));
}

void sendMastermindStateRequest() {
    uint32_t gameId = 0;
    uint32_t revision = 0;
    uint32_t digest = 0;
    portENTER_CRITICAL(&gameMux);
    if (mastermindStateReady) {
        gameId = mastermindState.gameId;
        revision = mastermindState.revision;
        digest = mastermindStateDigest(mastermindState);
    }
    portEXIT_CRITICAL(&gameMux);
    if (protocolMutex == nullptr ||
        xSemaphoreTake(protocolMutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    MastermindStateRequestPacket packet{
        makeHeader(MessageType::MastermindRequestState), gameId, revision,
        digest};
    esp_now_send(expectedPeerAddress, reinterpret_cast<uint8_t*>(&packet),
                 sizeof(packet));
    xSemaphoreGive(protocolMutex);
}

void sendCountdownStateIfCurrent(MessageType type,
                                 const CountdownWireState& state) {
    CountdownStatePacket packet{makeHeader(type), state};
    if (protocolMutex == nullptr ||
        xSemaphoreTake(protocolMutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&gameMux);
    const bool snapshotMatches =
        countdownStateReady && sameCountdownWireState(countdownState, state);
    const bool terminalDelivery =
        type == MessageType::CountdownState &&
        state.phase == CountdownWirePhase::Exited;
    const bool shouldSend =
        shouldSendActiveGameState(activeGame, ActiveGameKind::Countdown,
                                  state.gameId, snapshotMatches,
                                  terminalDelivery);
    portEXIT_CRITICAL(&gameMux);
    if (shouldSend) {
        esp_now_send(expectedPeerAddress, reinterpret_cast<uint8_t*>(&packet),
                     sizeof(packet));
    }
    xSemaphoreGive(protocolMutex);
}

void sendCountdownAck(const CountdownPendingAck& ack) {
    CountdownAckPacket packet{makeHeader(MessageType::CountdownAck),
                              ack.targetBoardId,
                              ack.gameId,
                              ack.revision,
                              ack.stateDigest,
                              ack.acknowledgedType,
                              {0, 0, 0}};
    esp_now_send(expectedPeerAddress, reinterpret_cast<uint8_t*>(&packet),
                 sizeof(packet));
}

void sendCountdownStateRequest() {
    uint32_t gameId = 0;
    uint32_t revision = 0;
    uint32_t digest = 0;
    portENTER_CRITICAL(&gameMux);
    if (countdownStateReady) {
        gameId = countdownState.gameId;
        revision = countdownState.revision;
        digest = countdownStateDigest(countdownState);
    }
    portEXIT_CRITICAL(&gameMux);
    CountdownStateRequestPacket packet{
        makeHeader(MessageType::CountdownRequestState), gameId, revision,
        digest};
    esp_now_send(expectedPeerAddress, reinterpret_cast<uint8_t*>(&packet),
                 sizeof(packet));
}

void refreshPeerIdentity() {
    uint32_t peerId = 0;
    portENTER_CRITICAL(&linkMux);
    peerId = linkState.peerBoardId;
    portEXIT_CRITICAL(&linkMux);
    if (peerId == 0 || gamePeerBoardId != 0) {
        return;
    }
    portENTER_CRITICAL(&gameMux);
    gamePeerBoardId = peerId;
    displayDirty = true;
    requestStateSoon = true;
    mastermindRequestStateSoon = true;
    portEXIT_CRITICAL(&gameMux);
    Serial.printf("PEER bound id=%08lX role=%s\n",
                  static_cast<unsigned long>(peerId),
                  boardId < peerId ? "host" : "guest");
}

bool mapTouch(int16_t& x, int16_t& y, TS_Point& point) {
    if (!touch.touched()) {
        return false;
    }
    point = touch.getPoint();
    x = constrain(map(point.x, kTouchMinX, kTouchMaxX, 0, display.width() - 1),
                  0, display.width() - 1);
    y = constrain(map(point.y, kTouchMinY, kTouchMaxY, 0,
                      display.height() - 1),
                  0, display.height() - 1);
    return true;
}

int8_t puzzlePositionAt(const PuzzleState& game, int16_t x, int16_t y) {
    const PuzzleLayout layout = puzzleLayout(game);
    if (x < layout.x || y < layout.y) {
        return -1;
    }
    const int16_t column =
        (x - layout.x) / (layout.tileWidth + kTileGap);
    const int16_t row =
        (y - layout.y) / (layout.tileHeight + kTileGap);
    if (column < 0 || column >= game.columns || row < 0 ||
        row >= game.rows) {
        return -1;
    }
    const int16_t localX =
        (x - layout.x) % (layout.tileWidth + kTileGap);
    const int16_t localY =
        (y - layout.y) % (layout.tileHeight + kTileGap);
    if (localX >= layout.tileWidth || localY >= layout.tileHeight) {
        return -1;
    }
    return row * game.columns + column;
}

void startPuzzle(const PuzzleSpec& spec) {
    PuzzleState started{};
    portENTER_CRITICAL(&gameMux);
    if (screenMode != ScreenMode::Home && activeGame.kind != ActiveGameKind::Home) {
        portEXIT_CRITICAL(&gameMux);
        return;
    }
    const uint32_t nextGameId = nextActiveGameEpoch(
        activeGame.epoch, stateReady ? puzzleState.gameId : 0,
        mastermindStateReady ? mastermindState.gameId : 0);
    if (nextGameId == kNoActiveGameEpoch) {
        portEXIT_CRITICAL(&gameMux);
        return;
    }
    started = makeScrambledPuzzle(std::min(boardId, gamePeerBoardId),
                                  nextGameId, spec);
    puzzleState = started;
    stateReady = true;
    activeGame = {nextGameId, ActiveGameKind::Puzzle};
    screenMode = ScreenMode::Puzzle;
    mastermindPendingDelivery.active = false;
    mastermindReconciliationPending = false;
    mastermindRequestStateSoon = false;
    sendMastermindFullStateSoon = false;
    pendingDelivery =
        PendingDelivery{true, MessageType::FullState, started, 0};
    displayDirty = true;
    portEXIT_CRITICAL(&gameMux);
    Serial.printf("GAME started id=%lu\n",
                  static_cast<unsigned long>(started.gameId));
}

bool commitMastermindState(const MastermindState& next, MessageType type,
                           const MastermindState* expected = nullptr) {
    portENTER_CRITICAL(&gameMux);
    if ((expected != nullptr &&
         (activeGame.kind != ActiveGameKind::Mastermind ||
          !mastermindStateReady ||
          !sameMastermindState(mastermindState, *expected))) ||
        (expected == nullptr &&
         ((screenMode != ScreenMode::Home && activeGame.kind != ActiveGameKind::Home) ||
          next.gameId <= activeGame.epoch))) {
        portEXIT_CRITICAL(&gameMux);
        return false;
    }
    mastermindState = next;
    mastermindStateReady = true;
    activeGame = {
        next.gameId,
        next.phase == MastermindPhase::Exited ? ActiveGameKind::Home
                                               : ActiveGameKind::Mastermind,
    };
    screenMode = next.phase == MastermindPhase::Exited
                     ? ScreenMode::Home
                     : ScreenMode::Mastermind;
    pendingDelivery.active = false;
    reconciliationPending = false;
    requestStateSoon = false;
    sendFullStateSoon = false;
    sendMastermindFullStateSoon = false;
    mastermindPendingDelivery =
        MastermindPendingDelivery{true, type, next, 0};
    noteMastermindPhase(next);
    displayDirty = true;
    portEXIT_CRITICAL(&gameMux);
    return true;
}

void startMastermind() {
    uint32_t nextGameId = kNoActiveGameEpoch;
    portENTER_CRITICAL(&gameMux);
    nextGameId = nextActiveGameEpoch(
        activeGame.epoch, stateReady ? puzzleState.gameId : 0,
        mastermindStateReady ? mastermindState.gameId : 0);
    portEXIT_CRITICAL(&gameMux);
    if (nextGameId == kNoActiveGameEpoch) {
        return;
    }
    const MastermindState started = makeMastermindMatch(
        std::min(boardId, gamePeerBoardId),
        std::max(boardId, gamePeerBoardId), nextGameId);
    if (!commitMastermindState(started,
                               MessageType::MastermindFullState)) {
        return;
    }
    Serial.printf("MASTERMIND started id=%lu\n",
                  static_cast<unsigned long>(started.gameId));
}

void startCountdown() {
    uint32_t nextGameId = kNoActiveGameEpoch;
    portENTER_CRITICAL(&gameMux);
    if (screenMode != ScreenMode::Home && activeGame.kind != ActiveGameKind::Home) {
        portEXIT_CRITICAL(&gameMux);
        return;
    }
    nextGameId = nextActiveGameEpoch(
        activeGame.epoch, stateReady ? puzzleState.gameId : 0,
        mastermindStateReady ? mastermindState.gameId : 0);
    if (nextGameId == kNoActiveGameEpoch) {
        portEXIT_CRITICAL(&gameMux);
        return;
    }
    countdownEngine.resetMatch(std::min(boardId, gamePeerBoardId),
                               std::max(boardId, gamePeerBoardId));
    const CountdownWireState started = makeCountdownMatch(
        std::min(boardId, gamePeerBoardId),
        std::max(boardId, gamePeerBoardId), nextGameId);
    countdownState = started;
    countdownStateReady = true;
    activeGame = {nextGameId, ActiveGameKind::Countdown};
    screenMode = ScreenMode::Countdown;
    countdownPendingDelivery =
        CountdownPendingDelivery{true, MessageType::CountdownFullState,
                                 started, 0};
    displayDirty = true;
    portEXIT_CRITICAL(&gameMux);
    Serial.printf("COUNTDOWN started id=%lu\n",
                  static_cast<unsigned long>(nextGameId));
}

void handleMastermindEditorTouch(int16_t x, int16_t y,
                                 bool secretEntry) {
    if (secretEntry && y >= 65 && y <= 112) {
        for (uint8_t peg = 0; peg < kMastermindCodeLength; ++peg) {
            if (abs(x - (70 + peg * 60)) <= 24) {
                selectedPeg = peg;
                displayDirty = true;
                return;
            }
        }
    } else if (!secretEntry && y >= 140 && y <= 176) {
        for (uint8_t peg = 0; peg < kMastermindCodeLength; ++peg) {
            if (abs(x - (215 + peg * 28)) <= 15) {
                selectedPeg = peg;
                displayDirty = true;
                return;
            }
        }
    }
    if (secretEntry && y >= 124 && y <= 162) {
        for (uint8_t color = 1; color <= kMastermindColorCount; ++color) {
            if (abs(x - (45 + (color - 1) * 46)) <= 18) {
                draftCode.colors[selectedPeg] = color;
                selectedPeg = (selectedPeg + 1) % kMastermindCodeLength;
                displayDirty = true;
                return;
            }
        }
    } else if (!secretEntry && x >= 201 && y >= 59 && y <= 130) {
        const uint8_t column = constrain((x - 201) / 38, 0, 2);
        const uint8_t row = constrain((y - 59) / 38, 0, 1);
        draftCode.colors[selectedPeg] = row * 3 + column + 1;
        selectedPeg = (selectedPeg + 1) % kMastermindCodeLength;
        displayDirty = true;
    }
}

void handleMastermindTouch(int16_t x, int16_t y) {
    MastermindState snapshot{};
    bool pending = false;
    portENTER_CRITICAL(&gameMux);
    snapshot = mastermindState;
    pending = mastermindPendingDelivery.active;
    portEXIT_CRITICAL(&gameMux);
    if (mastermindStateReady && x >= 270 && y <= 34) {
        MastermindState exited = snapshot;
        if (exitMastermindMatch(exited, boardId)) {
            commitMastermindState(exited, MessageType::MastermindState,
                                  &snapshot);
        }
        return;
    }
    if (!mastermindStateReady || pending || !peerOnline() ||
        snapshot.phase == MastermindPhase::RoundComplete) {
        return;
    }
    if (snapshot.phase == MastermindPhase::SecretEntry &&
        snapshot.codemakerBoardId == boardId) {
        handleMastermindEditorTouch(x, y, true);
        if (x >= 95 && x <= 225 && y >= 181 && y <= 223) {
            MastermindState next = snapshot;
            if (submitMastermindSecret(next, draftCode, boardId)) {
                commitMastermindState(next, MessageType::MastermindState,
                                      &snapshot);
            }
        }
    } else if (snapshot.phase == MastermindPhase::Guessing &&
               snapshot.codemakerBoardId != boardId) {
        handleMastermindEditorTouch(x, y, false);
        if (x >= 211 && x <= 303 && y >= 185 && y <= 224) {
            MastermindState next = snapshot;
            if (submitMastermindGuess(next, draftCode, boardId)) {
                lastSubmittedGuess = draftCode;
                commitMastermindState(next, MessageType::MastermindState,
                                      &snapshot);
            }
        }
    }
}

void handleTouch() {
    int16_t x = 0;
    int16_t y = 0;
    TS_Point point;
    const bool down = mapTouch(x, y, point);
    if (!down) {
        touchWasDown = false;
        return;
    }
    if (touchWasDown) {
        return;
    }
    touchWasDown = true;
    Serial.printf("TOUCH raw=%d,%d screen=%d,%d\n", point.x, point.y, x, y);

    ScreenMode mode;
    bool pending;
    PuzzleState snapshot{};
    portENTER_CRITICAL(&gameMux);
    mode = screenMode;
    pending = pendingDelivery.active;
    snapshot = puzzleState;
    portEXIT_CRITICAL(&gameMux);

    const bool exitPressed =
        x >= kExitX && x < kExitX + kExitWidth && y >= kExitY &&
        y < kExitY + kExitHeight;
    if (mode == ScreenMode::Mastermind && exitPressed) {
        MastermindState mastermindSnapshot{};
        portENTER_CRITICAL(&gameMux);
        mastermindSnapshot = mastermindState;
        portEXIT_CRITICAL(&gameMux);
        if (mastermindStateReady) {
            MastermindState exited = mastermindSnapshot;
            if (exitMastermindMatch(exited, boardId)) {
                commitMastermindState(exited, MessageType::MastermindState,
                                      &mastermindSnapshot);
            }
        }
        return;
    }
    if (mode == ScreenMode::Countdown && exitPressed) {
        CountdownWireState countdownSnapshot{};
        portENTER_CRITICAL(&gameMux);
        countdownSnapshot = countdownState;
        portEXIT_CRITICAL(&gameMux);
        if (countdownStateReady) {
            CountdownWireState exited = countdownSnapshot;
            if (exitCountdownMatch(exited, boardId)) {
                portENTER_CRITICAL(&gameMux);
                countdownState = exited;
                countdownStateReady = true;
                activeGame = {exited.gameId, ActiveGameKind::Home};
                screenMode = ScreenMode::Home;
                countdownPendingDelivery =
                    CountdownPendingDelivery{true, MessageType::CountdownState,
                                             exited, 0};
                displayDirty = true;
                portEXIT_CRITICAL(&gameMux);
            }
        }
        return;
    }
    // --- NumPicking: chooser taps a large-count button (0-4) then CONFIRM ---
    if (mode == ScreenMode::Countdown &&
        static_cast<CountdownRoundSubPhase>(countdownState.roundSubPhase) ==
            CountdownRoundSubPhase::NumPicking) {
        // Large-count buttons: x = 16 + i*58, y = kCdLcY, w=kCdLcW, h=kCdLcH
        if (y >= kCdLcY && y < kCdLcY + kCdLcH) {
            for (uint8_t i = 0; i <= 4; ++i) {
                int16_t bx = static_cast<int16_t>(16 + i * 58);
                if (x >= bx && x < bx + kCdLcW &&
                    boardId == countdownState.chooserBoardId) {
                    numUi.largeCount = i;
                    displayDirty = true;
                    break;
                }
            }
        }
        // CONFIRM button: x=95..225, y=185..221
        if (y >= 185 && y < 221 && x >= 95 && x < 225 &&
            boardId == countdownState.chooserBoardId) {
            // Chooser confirmed large count: transition to NumThinking
            // Host starts the round in the engine; both advance sub-phase
            CountdownWireState snap{};
            portENTER_CRITICAL(&gameMux);
            snap = countdownState;
            portEXIT_CRITICAL(&gameMux);
            if (countdownStateReady && boardId == snap.hostBoardId) {
                auto random = flip7::countdown::makeRoundRandom(
                    snap.gameId, snap.roundNumber);
                portENTER_CRITICAL(&gameMux);
                countdownEngine.startNextRound(
                    flip7::countdown::RoundType::Numbers, random,
                    numUi.largeCount);
                portEXIT_CRITICAL(&gameMux);

                CountdownWireState advanced = snap;
                if (advanceCountdownSubPhase(
                        advanced, boardId,
                        CountdownRoundSubPhase::NumThinking)) {
                    // Store largeCount in roundConfig so the guest can
                    // reconstruct the same puzzle via makeRoundRandom.
                    advanced.roundConfig = numUi.largeCount;
                    roundPhaseStartMs = millis();
                    portENTER_CRITICAL(&gameMux);
                    if (sameCountdownWireState(countdownState, snap)) {
                        countdownState = advanced;
                        countdownPendingDelivery = CountdownPendingDelivery{
                            true, MessageType::CountdownState, advanced, 0};
                        displayDirty = true;
                    }
                    portEXIT_CRITICAL(&gameMux);
                }
            }
        }
        return;
    }

    // --- NumClaimEntry: numpad interaction ---
    if (mode == ScreenMode::Countdown &&
        static_cast<CountdownRoundSubPhase>(countdownState.roundSubPhase) ==
            CountdownRoundSubPhase::NumClaimEntry &&
        !numUi.claimSent) {
        // Numpad rows: y = kCdPadY + row*(kCdPadH+kCdPadGap)
        // Cols: x = kCdPadX + col*(kCdPadW+kCdPadGap)
        for (int row = 0; row < 4; ++row) {
            int16_t by = static_cast<int16_t>(kCdPadY + row * (kCdPadH + kCdPadGap));
            if (y < by || y >= by + kCdPadH) continue;
            for (int col = 0; col < 3; ++col) {
                int16_t bx = static_cast<int16_t>(kCdPadX + col * (kCdPadW + kCdPadGap));
                if (x < bx || x >= bx + kCdPadW) continue;
                const int digits[4][3] = {{7,8,9},{4,5,6},{1,2,3},{0,-1,-2}};
                int d = digits[row][col];
                if (d >= 0) {
                    int32_t next = numUi.draftValue * 10 + d;
                    if (next <= 999) numUi.draftValue = next;
                } else if (d == -1) {  // CLR
                    numUi.draftValue = 0;
                } else {  // SUBMIT (d==-2)
                    // Send claim to engine (host) — locally mark sent
                    portENTER_CRITICAL(&gameMux);
                    auto* nr = countdownEngine.numbersRound();
                    if (nr) nr->submitClaim(boardId, numUi.draftValue);
                    portEXIT_CRITICAL(&gameMux);
                    numUi.claimSent = true;
                    // If host: also check if both claims are in and advance
                    // (guest claim will come via action packet — handled later)
                }
                displayDirty = true;
                break;
            }
            break;
        }
        return;
    }

    // -----------------------------------------------------------------------
    // Letters sub-phase touch handlers
    // -----------------------------------------------------------------------

    // LetPicking: chooser taps vowel/consonant letter buttons or LOCK IN
    if (mode == ScreenMode::Countdown &&
        static_cast<CountdownRoundSubPhase>(countdownState.roundSubPhase) ==
            CountdownRoundSubPhase::LetPicking &&
        boardId == countdownState.chooserBoardId) {

        flip7::countdown::LettersRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.lettersProjection();
        portEXIT_CRITICAL(&gameMux);

        // Tab toggle: VOWELS (x=5..105, y=50..78) / CONSONANTS (x=215..315)
        if (y >= 50 && y < 78) {
            if (x >= 5 && x < 105) { letUi.showConsonants = false; displayDirty = true; return; }
            if (x >= 215 && x < 315) { letUi.showConsonants = true; displayDirty = true; return; }
        }

        char picked = 0;
        bool isVowel = false;

        if (!letUi.showConsonants) {
            // Vowel buttons: A E I O U at x=10+i*60, y=95..139, W=50, H=44
            if (y >= 95 && y < 139) {
                const char vowels[5] = {'A','E','I','O','U'};
                for (int i = 0; i < 5; ++i) {
                    int16_t bx = static_cast<int16_t>(10 + i * 60);
                    if (x >= bx && x < bx + 50) {
                        picked  = vowels[i];
                        isVowel = true;
                        break;
                    }
                }
            }
        } else {
            // Consonant keyboard: BCDFGH / JKLMNP / QRSTVW / XYZ
            const char rows[4][7] = {
                {'B','C','D','F','G','H', 0},
                {'J','K','L','M','N','P', 0},
                {'Q','R','S','T','V','W', 0},
                {'X','Y','Z', 0, 0, 0,  0},
            };
            const int rowLen[4] = {6, 6, 6, 3};
            constexpr int16_t W = 42, H = 32, GAP = 4;
            for (int row = 0; row < 4; ++row) {
                int16_t startX = static_cast<int16_t>(
                    (320 - rowLen[row] * (W + GAP) + GAP) / 2);
                int16_t by = static_cast<int16_t>(88 + row * (H + GAP));
                if (y < by || y >= by + H) continue;
                for (int col = 0; col < rowLen[row]; ++col) {
                    int16_t bx = static_cast<int16_t>(startX + col * (W + GAP));
                    if (x >= bx && x < bx + W && rows[row][col] != 0) {
                        picked  = rows[row][col];
                        isVowel = false;
                        break;
                    }
                }
                if (picked) break;
            }
        }

        if (picked && proj.letterCount < 9) {
            // Apply to engine and sync via wire
            flip7::countdown::CommandContext ctx{boardId, 0};
            std::vector<uint8_t> payload = {static_cast<uint8_t>(picked)};
            portENTER_CRITICAL(&gameMux);
            auto* lr = countdownEngine.lettersRound();
            if (lr) {
                lr->applyCommand(ctx,
                    isVowel ? flip7::countdown::CommandType::LetPickVowel
                             : flip7::countdown::CommandType::LetPickConsonant,
                    payload);
            }
            portEXIT_CRITICAL(&gameMux);
            displayDirty = true;

            // Auto-advance to LetThinking when 9 letters are drawn (host only)
            portENTER_CRITICAL(&gameMux);
            auto proj2 = countdownEngine.lettersProjection();
            bool done  = (proj2.letterCount >= 9);
            portEXIT_CRITICAL(&gameMux);
            if (done && boardId == countdownState.hostBoardId) {
                CountdownWireState snap{};
                portENTER_CRITICAL(&gameMux);
                snap = countdownState;
                portEXIT_CRITICAL(&gameMux);
                CountdownWireState adv = snap;
                if (advanceCountdownSubPhase(adv, boardId,
                        CountdownRoundSubPhase::LetThinking)) {
                    roundPhaseStartMs = millis();
                    portENTER_CRITICAL(&gameMux);
                    if (sameCountdownWireState(countdownState, snap)) {
                        countdownState = adv;
                        countdownPendingDelivery = CountdownPendingDelivery{
                            true, MessageType::CountdownState, adv, 0};
                        displayDirty = true;
                    }
                    portEXIT_CRITICAL(&gameMux);
                }
            }
        }

        // LOCK IN button: x=95..225, y=200..234 (when 9 letters)
        if (proj.letterCount >= 9 && y >= 200 && y < 234 &&
            x >= 95 && x < 225 && boardId == countdownState.hostBoardId) {
            CountdownWireState snap{};
            portENTER_CRITICAL(&gameMux);
            snap = countdownState;
            portEXIT_CRITICAL(&gameMux);
            CountdownWireState adv = snap;
            if (advanceCountdownSubPhase(adv, boardId,
                    CountdownRoundSubPhase::LetThinking)) {
                roundPhaseStartMs = millis();
                portENTER_CRITICAL(&gameMux);
                if (sameCountdownWireState(countdownState, snap)) {
                    countdownState = adv;
                    countdownPendingDelivery = CountdownPendingDelivery{
                        true, MessageType::CountdownState, adv, 0};
                    displayDirty = true;
                }
                portEXIT_CRITICAL(&gameMux);
            }
        }
        return;
    }

    // LetClaimEntry: digit picker (1-9) + SUBMIT CLAIM
    if (mode == ScreenMode::Countdown &&
        static_cast<CountdownRoundSubPhase>(countdownState.roundSubPhase) ==
            CountdownRoundSubPhase::LetClaimEntry &&
        !letUi.claimSent) {

        // Digit row: y=130..164, x = (320 - 9*31+3)/2 + (d-1)*31
        constexpr int16_t DW = 28, DH = 34, DY = 130, DGAP = 3;
        int16_t startX = static_cast<int16_t>((320 - 9*(DW+DGAP)+DGAP) / 2);
        if (y >= DY && y < DY + DH) {
            for (int d = 1; d <= 9; ++d) {
                int16_t bx = static_cast<int16_t>(startX + (d-1)*(DW+DGAP));
                if (x >= bx && x < bx + DW) {
                    letUi.claimLength = static_cast<uint8_t>(d);
                    displayDirty = true;
                    return;
                }
            }
        }
        // SUBMIT CLAIM button: x=95..225, y=180..214
        if (y >= 180 && y < 214 && x >= 95 && x < 225 && letUi.claimLength > 0) {
            portENTER_CRITICAL(&gameMux);
            auto* lr = countdownEngine.lettersRound();
            if (lr) lr->submitClaim(boardId, letUi.claimLength);
            portEXIT_CRITICAL(&gameMux);
            letUi.claimSent = true;
            displayDirty = true;
        }
        return;
    }

    // LetPresenting: tile taps, CLR, PRESENT WORD
    if (mode == ScreenMode::Countdown &&
        (static_cast<CountdownRoundSubPhase>(countdownState.roundSubPhase) ==
             CountdownRoundSubPhase::LetPresentPlayerA ||
         static_cast<CountdownRoundSubPhase>(countdownState.roundSubPhase) ==
             CountdownRoundSubPhase::LetPresentPlayerB) &&
        !letUi.presentationSent) {

        auto sp = static_cast<CountdownRoundSubPhase>(
            countdownState.roundSubPhase);
        bool aPhase = (sp == CountdownRoundSubPhase::LetPresentPlayerA);
        bool isPresenter =
            (aPhase && letUi.presenterIsHost &&
             boardId == countdownState.hostBoardId) ||
            (aPhase && !letUi.presenterIsHost &&
             boardId == countdownState.guestBoardId) ||
            (!aPhase && letUi.presenterIsHost &&
             boardId == countdownState.guestBoardId) ||
            (!aPhase && !letUi.presenterIsHost &&
             boardId == countdownState.hostBoardId);
        if (!isPresenter) return;

        flip7::countdown::LettersRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.lettersProjection();
        portEXIT_CRITICAL(&gameMux);

        // Letter tile row: y=kCdTileY..kCdTileY+34
        if (y >= kCdTileY && y < kCdTileY + 34 && letUi.wordLen < 9) {
            constexpr int16_t W = 30, GAP = 4;
            int16_t startX = static_cast<int16_t>(
                (320 - proj.letterCount * (W + GAP) + GAP) / 2);
            for (uint8_t ti = 0; ti < proj.letterCount; ++ti) {
                int16_t tx = static_cast<int16_t>(startX + ti * (W + GAP));
                if (x >= tx && x < tx + W) {
                    letUi.word[letUi.wordLen++] = proj.letters[ti];
                    displayDirty = true;
                    return;
                }
            }
        }
        // CLR button: x=250..314, y=98..132
        if (y >= 98 && y < 132 && x >= 250 && x < 314 && letUi.wordLen > 0) {
            letUi.wordLen--;
            displayDirty = true;
            return;
        }
        // PRESENT WORD button: x=95..225, y=155..189
        if (y >= 155 && y < 189 && x >= 95 && x < 225 && letUi.wordLen > 0) {
            flip7::countdown::CommandContext ctx{boardId, 0};
            std::string wstr(letUi.word, letUi.wordLen);
            std::vector<uint8_t> payload(wstr.begin(), wstr.end());
            portENTER_CRITICAL(&gameMux);
            auto* lr = countdownEngine.lettersRound();
            if (lr) lr->applyCommand(ctx,
                flip7::countdown::CommandType::LetPresentComplete, payload);
            portEXIT_CRITICAL(&gameMux);
            letUi.presentationSent = true;
            displayDirty = true;
        }
        return;
    }

    // LetVerification: YES/NO buttons (verifier only)
    if (mode == ScreenMode::Countdown &&
        (static_cast<CountdownRoundSubPhase>(countdownState.roundSubPhase) ==
             CountdownRoundSubPhase::LetPresentPlayerA ||
         static_cast<CountdownRoundSubPhase>(countdownState.roundSubPhase) ==
             CountdownRoundSubPhase::LetPresentPlayerB) &&
        letUi.presentationSent && !letUi.verificationSent) {

        auto sp = static_cast<CountdownRoundSubPhase>(
            countdownState.roundSubPhase);
        bool aPhase = (sp == CountdownRoundSubPhase::LetPresentPlayerA);
        bool presenterIsHost = letUi.presenterIsHost;
        bool isVerifier =
            !((aPhase && presenterIsHost && boardId == countdownState.hostBoardId) ||
              (aPhase && !presenterIsHost && boardId == countdownState.guestBoardId) ||
              (!aPhase && presenterIsHost && boardId == countdownState.guestBoardId) ||
              (!aPhase && !presenterIsHost && boardId == countdownState.hostBoardId));
        if (!isVerifier) return;

        // YES: x=20..140, y=178..222
        if (y >= 178 && y < 222 && x >= 20 && x < 140) {
            letUi.verificationAnswer = true;
            letUi.verificationSent   = true;
            flip7::countdown::CommandContext ctx{boardId, 0};
            std::vector<uint8_t> payload = {1};
            portENTER_CRITICAL(&gameMux);
            auto* lr = countdownEngine.lettersRound();
            if (lr) lr->applyCommand(ctx,
                flip7::countdown::CommandType::LetVerifyWord, payload);
            portEXIT_CRITICAL(&gameMux);
            // Host advances to LetResult or PlayerB
            if (boardId == countdownState.hostBoardId) {
                CountdownWireState snap{};
                portENTER_CRITICAL(&gameMux);
                snap = countdownState;
                portEXIT_CRITICAL(&gameMux);
                CountdownWireState adv = snap;
                auto nextSP = aPhase ? CountdownRoundSubPhase::LetPresentPlayerB
                                     : CountdownRoundSubPhase::LetResult;
                if (advanceCountdownSubPhase(adv, boardId, nextSP)) {
                    roundPhaseStartMs = millis();
                    letUi = LetUiState{};  // reset for next presenter
                    portENTER_CRITICAL(&gameMux);
                    if (sameCountdownWireState(countdownState, snap)) {
                        countdownState = adv;
                        countdownPendingDelivery = CountdownPendingDelivery{
                            true, MessageType::CountdownState, adv, 0};
                        displayDirty = true;
                    }
                    portEXIT_CRITICAL(&gameMux);
                }
            }
            displayDirty = true;
            return;
        }
        // NO: x=180..300, y=178..222
        if (y >= 178 && y < 222 && x >= 180 && x < 300) {
            letUi.verificationAnswer = false;
            letUi.verificationSent   = true;
            flip7::countdown::CommandContext ctx{boardId, 0};
            std::vector<uint8_t> payload = {0};
            portENTER_CRITICAL(&gameMux);
            auto* lr = countdownEngine.lettersRound();
            if (lr) lr->applyCommand(ctx,
                flip7::countdown::CommandType::LetVerifyWord, payload);
            portEXIT_CRITICAL(&gameMux);
            if (boardId == countdownState.hostBoardId) {
                CountdownWireState snap{};
                portENTER_CRITICAL(&gameMux);
                snap = countdownState;
                portEXIT_CRITICAL(&gameMux);
                CountdownWireState adv = snap;
                auto nextSP = aPhase ? CountdownRoundSubPhase::LetPresentPlayerB
                                     : CountdownRoundSubPhase::LetResult;
                if (advanceCountdownSubPhase(adv, boardId, nextSP)) {
                    roundPhaseStartMs = millis();
                    letUi = LetUiState{};
                    portENTER_CRITICAL(&gameMux);
                    if (sameCountdownWireState(countdownState, snap)) {
                        countdownState = adv;
                        countdownPendingDelivery = CountdownPendingDelivery{
                            true, MessageType::CountdownState, adv, 0};
                        displayDirty = true;
                    }
                    portEXIT_CRITICAL(&gameMux);
                }
            }
            displayDirty = true;
            return;
        }
        return;
    }

    // -----------------------------------------------------------------------
    // Conundrum touch handlers
    // -----------------------------------------------------------------------

    if (mode == ScreenMode::Countdown &&
        static_cast<CountdownRoundSubPhase>(countdownState.roundSubPhase) ==
            CountdownRoundSubPhase::ConActive) {

        constexpr int16_t CX = 160, CY = 125, CR = 75;
        constexpr int16_t TW = 26, TH = 26;

        // Reshuffle button: x=284..314, y=38..60
        if (y >= 38 && y < 60 && x >= 284 && x < 314) {
            // Fisher-Yates shuffle using millis() as entropy
            uint32_t seed = millis();
            for (int i = 8; i > 0; --i) {
                seed = seed * 1664525u + 1013904223u;
                int j = seed % static_cast<uint32_t>(i + 1);
                uint8_t tmp = conUi.circleOrder[i];
                conUi.circleOrder[i] = conUi.circleOrder[j];
                conUi.circleOrder[j] = tmp;
            }
            displayDirty = true;
            return;
        }

        // Letter circles: check if tap is within TW/2 of any circle centre
        flip7::countdown::ConundrumRoundProjection proj{};
        portENTER_CRITICAL(&gameMux);
        proj = countdownEngine.conundrumProjection();
        portEXIT_CRITICAL(&gameMux);

        for (uint8_t i = 0; i < 9; ++i) {
            // Skip already selected
            bool alreadySel = false;
            for (uint8_t s = 0; s < conUi.selectedCount; ++s)
                if (conUi.selected[s] == i) { alreadySel = true; break; }
            if (alreadySel) continue;

            int16_t tx, ty;
            conCirclePos(i, conUi.circleOrder, CX, CY, CR, tx, ty);
            if (abs(x - tx) <= TW/2 + 4 && abs(y - ty) <= TH/2 + 4) {
                conUi.selected[conUi.selectedCount++] = i;
                displayDirty = true;

                if (conUi.selectedCount == 9) {
                    // Build attempt string and submit
                    char attempt[10]{};
                    for (uint8_t s = 0; s < 9; ++s)
                        attempt[s] = proj.scramble[conUi.circleOrder[conUi.selected[s]]];

                    flip7::countdown::CommandContext ctx{boardId, 0};
                    std::vector<uint8_t> payload(attempt, attempt + 9);
                    bool correct = false;
                    portENTER_CRITICAL(&gameMux);
                    auto* cr = countdownEngine.conundrumRound();
                    if (cr) {
                        auto res = cr->applyCommand(ctx,
                            flip7::countdown::CommandType::ConSubmitAttempt,
                            payload);
                        correct = res.accepted;
                    }
                    portEXIT_CRITICAL(&gameMux);

                    if (correct && boardId == countdownState.hostBoardId) {
                        // Advance to ConResult
                        CountdownWireState snap{};
                        portENTER_CRITICAL(&gameMux);
                        snap = countdownState;
                        portEXIT_CRITICAL(&gameMux);
                        CountdownWireState adv = snap;
                        if (advanceCountdownSubPhase(adv, boardId,
                                CountdownRoundSubPhase::ConResult)) {
                            portENTER_CRITICAL(&gameMux);
                            if (sameCountdownWireState(countdownState, snap)) {
                                countdownState = adv;
                                countdownPendingDelivery = CountdownPendingDelivery{
                                    true, MessageType::CountdownState, adv, 0};
                                displayDirty = true;
                            }
                            portEXIT_CRITICAL(&gameMux);
                        }
                    } else if (!correct) {
                        // Reset attempt — forced local reshuffle
                        conUi.selectedCount = 0;
                        conUi.showGoodTry   = true;
                        conUi.goodTryMs     = millis();
                        // Shuffle circle order
                        uint32_t seed2 = millis();
                        for (int si = 8; si > 0; --si) {
                            seed2 = seed2 * 1664525u + 1013904223u;
                            int j = seed2 % static_cast<uint32_t>(si + 1);
                            uint8_t tmp = conUi.circleOrder[si];
                            conUi.circleOrder[si] = conUi.circleOrder[j];
                            conUi.circleOrder[j] = tmp;
                        }
                    }
                }
                return;
            }
        }
        return;
    }
    if (mode == ScreenMode::Countdown &&
        y >= kCountdownRoundTypeY &&
        y < kCountdownRoundTypeY + kCountdownRoundTypeHeight) {
        uint8_t roundType = 0;
        if (x >= kCountdownNumbersX &&
            x < kCountdownNumbersX + kCountdownRoundTypeWidth) {
            roundType = 1;  // Numbers
        } else if (x >= kCountdownLettersX &&
                   x < kCountdownLettersX + kCountdownRoundTypeWidth) {
            roundType = 2;  // Letters
        } else if (x >= kCountdownConundrumX &&
                   x < kCountdownConundrumX + kCountdownRoundTypeWidth) {
            roundType = 3;  // Conundrum
        }
        if (roundType != 0) {
            CountdownWireState countdownSnapshot{};
            portENTER_CRITICAL(&gameMux);
            countdownSnapshot = countdownState;
            portEXIT_CRITICAL(&gameMux);
            // Chooser (not host) picks the round type.
            if (countdownStateReady &&
                boardId == countdownSnapshot.chooserBoardId) {
                CountdownWireState selected = countdownSnapshot;
                if (selectCountdownRoundType(selected, boardId, roundType)) {
                    roundPhaseStartMs = millis();
                    numUi = NumUiState{};  // reset per-round UI state
                    portENTER_CRITICAL(&gameMux);
                    if (sameCountdownWireState(countdownState,
                                               countdownSnapshot)) {
                        countdownState = selected;
                        countdownStateReady = true;
                        countdownPendingDelivery = CountdownPendingDelivery{
                            true, MessageType::CountdownState, selected, 0};
                        displayDirty = true;
                    }
                    portEXIT_CRITICAL(&gameMux);
                }
            }
        }
        return;
    }
    if ((mode == ScreenMode::Puzzle || mode == ScreenMode::Complete) &&
        exitPressed) {
        PuzzleState exited{};
        bool accepted = false;
        portENTER_CRITICAL(&gameMux);
        if (stateReady) {
            exited = puzzleState;
            accepted = exitPuzzle(exited, boardId, gamePeerBoardId);
            if (accepted) {
                puzzleState = exited;
                pendingDelivery = PendingDelivery{
                    true, MessageType::PuzzleState, exited, 0};
                screenMode = ScreenMode::Home;
                completedAtMs = 0;
                reconciliationPending = false;
                displayDirty = true;
            }
        }
        portEXIT_CRITICAL(&gameMux);
        return;
    }

    if (mode == ScreenMode::Home) {
        if (localIsHost() && peerOnline() && !pending) {
            if (y >= 75 && y < 145) {
                if (x >= 10 && x < 155) {
                    startPuzzle({3, 3, PuzzleTheme::Planets});
                } else if (x >= 165 && x < 310) {
                    startPuzzle({4, 3, PuzzleTheme::Greek});
                }
            } else if (y >= 150 && y < 220) {
                if (x >= 10 && x < 155) {
                    startMastermind();
                } else if (x >= 165 && x < 310) {
                    startCountdown();
                }
            }
        }
        return;
    }
    if (mode == ScreenMode::Mastermind) {
        handleMastermindTouch(x, y);
        return;
    }
    if (mode == ScreenMode::Mastermind) {
        handleMastermindTouch(x, y);
        return;
    }
    if (mode != ScreenMode::Puzzle || pending || !peerOnline()) {
        return;
    }

    const int8_t position = puzzlePositionAt(snapshot, x, y);
    if (position < 0) {
        return;
    }
    PuzzleState moved{};
    bool accepted = false;
    uint8_t tile = 0;
    portENTER_CRITICAL(&gameMux);
    if (stateReady && screenMode == ScreenMode::Puzzle &&
        !pendingDelivery.active && puzzleState.gameId == snapshot.gameId &&
        puzzleState.revision == snapshot.revision &&
        puzzleState.phase == snapshot.phase &&
        puzzleState.turnBoardId == snapshot.turnBoardId &&
        samePuzzleSpec(puzzleSpec(puzzleState), puzzleSpec(snapshot))) {
        tile = puzzleState.tiles[position];
        accepted = tryPuzzleMove(puzzleState, position, boardId,
                                 gamePeerBoardId);
        if (accepted) {
            moved = puzzleState;
            pendingDelivery = PendingDelivery{
                true, MessageType::PuzzleState, moved, 0};
            if (isPuzzleSolved(moved)) {
                screenMode = ScreenMode::Complete;
                completedAtMs = millis();
            }
            displayDirty = true;
        }
    }
    portEXIT_CRITICAL(&gameMux);
    if (!accepted) {
        Serial.printf("MOVE ignored position=%d\n", position);
        return;
    }
    Serial.printf("LOCAL move symbol=%u game=%lu revision=%lu next=%08lX\n",
                  tile, static_cast<unsigned long>(moved.gameId),
                  static_cast<unsigned long>(moved.revision),
                  static_cast<unsigned long>(moved.turnBoardId));
}

void serviceProtocol(uint32_t now) {
    if (espNowReady && now - lastHeartbeatMs >= kHeartbeatIntervalMs) {
        lastHeartbeatMs = now;
        sendHeartbeat();
    }

    PendingDelivery delivery{};
    PendingAck ack{};
    PuzzleState fullState{};
    MastermindPendingDelivery mastermindDelivery{};
    MastermindPendingAck mastermindAck{};
    MastermindState mastermindFullState{};
    CountdownPendingDelivery countdownDelivery{};
    CountdownPendingAck countdownAck{};
    CountdownWireState countdownFullState{};
    bool sendDelivery = false;
    bool sendAckNow = false;
    bool sendFullNow = false;
    bool requestNow = false;
    bool sendMastermindDeliveryNow = false;
    bool sendMastermindAckNow = false;
    bool sendMastermindFullNow = false;
    bool requestMastermindNow = false;
    bool sendCountdownDeliveryNow = false;
    bool sendCountdownAckNow = false;
    bool sendCountdownFullNow = false;
    bool requestCountdownNow = false;
    portENTER_CRITICAL(&gameMux);
    const bool puzzleActive = activeGame.kind == ActiveGameKind::Puzzle;
    const bool mastermindActive =
        activeGame.kind == ActiveGameKind::Mastermind;
    const bool countdownActive =
        activeGame.kind == ActiveGameKind::Countdown;
    const bool discoveringGame = activeGame.kind == ActiveGameKind::Home;
    if (pendingDelivery.active &&
        now - pendingDelivery.lastSentMs >= kDeliveryRetryMs) {
        pendingDelivery.lastSentMs = now;
        delivery = pendingDelivery;
        sendDelivery = true;
    }
    if (pendingAck.active) {
        ack = pendingAck;
        pendingAck.active = false;
        sendAckNow = true;
    }
    if (sendFullStateSoon && stateReady && puzzleActive) {
        sendFullStateSoon = false;
        fullState = puzzleState;
        sendFullNow = true;
    }
    if ((puzzleActive || discoveringGame) &&
        (requestStateSoon ||
         (gamePeerBoardId != 0 && (!stateReady || reconciliationPending) &&
          now - lastStateRequestMs >= kStateRequestIntervalMs))) {
        requestStateSoon = false;
        requestNow = true;
    }
    if (mastermindPendingDelivery.active &&
        now - mastermindPendingDelivery.lastSentMs >= kDeliveryRetryMs) {
        mastermindPendingDelivery.lastSentMs = now;
        mastermindDelivery = mastermindPendingDelivery;
        sendMastermindDeliveryNow = true;
    }
    if (mastermindPendingAck.active) {
        mastermindAck = mastermindPendingAck;
        mastermindPendingAck.active = false;
        sendMastermindAckNow = true;
    }
    if (sendMastermindFullStateSoon && mastermindStateReady &&
        mastermindActive) {
        sendMastermindFullStateSoon = false;
        mastermindFullState = mastermindState;
        sendMastermindFullNow = true;
    }
    if ((mastermindActive || discoveringGame) &&
        (mastermindRequestStateSoon ||
         (gamePeerBoardId != 0 &&
          (!mastermindStateReady || mastermindReconciliationPending) &&
          now - lastStateRequestMs >= kStateRequestIntervalMs))) {
        mastermindRequestStateSoon = false;
        requestMastermindNow = true;
    }
    if (countdownPendingDelivery.active &&
        now - countdownPendingDelivery.lastSentMs >= kDeliveryRetryMs) {
        countdownPendingDelivery.lastSentMs = now;
        countdownDelivery = countdownPendingDelivery;
        sendCountdownDeliveryNow = true;
    }
    if (countdownPendingAck.active) {
        countdownAck = countdownPendingAck;
        countdownPendingAck.active = false;
        sendCountdownAckNow = true;
    }
    if (sendCountdownFullStateSoon && countdownStateReady &&
        countdownActive) {
        sendCountdownFullStateSoon = false;
        countdownFullState = countdownState;
        sendCountdownFullNow = true;
    }
    if ((countdownActive || discoveringGame) &&
        (countdownRequestStateSoon ||
         (gamePeerBoardId != 0 &&
          (!countdownStateReady || countdownReconciliationPending) &&
          now - lastStateRequestMs >= kStateRequestIntervalMs))) {
        countdownRequestStateSoon = false;
        requestCountdownNow = true;
    }
    if (requestNow || requestMastermindNow || requestCountdownNow) {
        lastStateRequestMs = now;
    }
    portEXIT_CRITICAL(&gameMux);

    if (sendDelivery) {
        sendPuzzleStateIfCurrent(delivery.type, delivery.state);
    }
    if (sendAckNow) {
        sendAck(ack);
    }
    if (sendFullNow) {
        sendPuzzleStateIfCurrent(MessageType::FullState, fullState);
    }
    if (requestNow) {
        sendStateRequest();
    }
    if (sendMastermindDeliveryNow) {
        sendMastermindStateIfCurrent(mastermindDelivery.type,
                                     mastermindDelivery.state);
    }
    if (sendMastermindAckNow) {
        sendMastermindAck(mastermindAck);
    }
    if (sendMastermindFullNow) {
        sendMastermindStateIfCurrent(MessageType::MastermindFullState,
                                     mastermindFullState);
    }
    if (requestMastermindNow) {
        sendMastermindStateRequest();
    }
    if (sendCountdownDeliveryNow) {
        sendCountdownStateIfCurrent(countdownDelivery.type,
                                    countdownDelivery.state);
    }
    if (sendCountdownAckNow) {
        sendCountdownAck(countdownAck);
    }
    if (sendCountdownFullNow) {
        sendCountdownStateIfCurrent(MessageType::CountdownFullState,
                                    countdownFullState);
    }
    if (requestCountdownNow) {
        sendCountdownStateRequest();
    }
}

void advanceMastermindRoundIfReady(uint32_t now) {
    MastermindState snapshot{};
    bool shouldAdvance = false;
    portENTER_CRITICAL(&gameMux);
    snapshot = mastermindState;
    shouldAdvance = localIsHost() && mastermindStateReady &&
        screenMode == ScreenMode::Mastermind &&
        snapshot.phase == MastermindPhase::RoundComplete &&
        !mastermindPendingDelivery.active && mastermindCompletedAtMs != 0 &&
        now - mastermindCompletedAtMs >= kMastermindCompleteDisplayMs;
    portEXIT_CRITICAL(&gameMux);
    const MastermindState expected = snapshot;
    if (shouldAdvance && advanceMastermindRound(snapshot, boardId)) {
        commitMastermindState(snapshot, MessageType::MastermindState,
                              &expected);
    }
}
}  // namespace

void setup() {
    Serial.begin(115200);
    pinMode(kBacklightPin, OUTPUT);
    digitalWrite(kBacklightPin, HIGH);

    display.init();
    display.setRotation(1);
    touchSpi.begin(kTouchClockPin, kTouchMisoPin, kTouchMosiPin, kTouchCsPin);
    touch.begin(touchSpi);
    touch.setRotation(1);

    boardId = static_cast<uint32_t>(ESP.getEfuseMac());
    do {
        bootSessionId = esp_random();
    } while (bootSessionId == 0);
    lastEspNowAttemptMs = millis();
    protocolMutex = xSemaphoreCreateMutex();
    espNowReady = protocolMutex != nullptr && startEspNow();
    Serial.printf("TWO-PLAYER GAMES ready mac=%s id=%08lX esp-now=%s\n",
                  WiFi.macAddress().c_str(),
                  static_cast<unsigned long>(boardId),
                  espNowReady ? "ready" : "failed");
    renderScreen();
}

void loop() {
    const uint32_t now = millis();
    if (!espNowReady && now - lastEspNowAttemptMs >= kEspNowRetryMs) {
        lastEspNowAttemptMs = now;
        espNowReady = startEspNow();
        Serial.printf("ESP-NOW retry=%s\n", espNowReady ? "ready" : "failed");
        portENTER_CRITICAL(&gameMux);
        displayDirty = true;
        portEXIT_CRITICAL(&gameMux);
    }
    refreshPeerIdentity();
    serviceProtocol(now);
    advanceMastermindRoundIfReady(now);

    // --- Countdown sub-phase auto-advance (host only) and animation ticks ---
    {
        CountdownWireState snap{};
        bool cdReady = false;
        bool cdHost  = false;
        portENTER_CRITICAL(&gameMux);
        snap    = countdownState;
        cdReady = countdownStateReady;
        cdHost  = (boardId == countdownState.hostBoardId);
        portEXIT_CRITICAL(&gameMux);

        if (cdReady && snap.phase == CountdownWirePhase::InRound) {
            using SP = CountdownRoundSubPhase;
            auto sp = static_cast<SP>(snap.roundSubPhase);

            // Rate-limited re-render during timed animation sub-phases (~10 fps).
            // Without the limit every 10ms loop tick causes a full fillScreen
            // redraw which flickers visibly on the TFT.
            // ConActive uses a slower rate (500ms) since tiles still need full redraws.
            bool animating  = (sp == SP::Intro      ||
                               sp == SP::NumThinking ||
                               sp == SP::LetThinking);
            bool conAnimating = (sp == SP::ConActive);
            if ((animating && (now - lastAnimFrameMs >= kAnimFrameIntervalMs ||
                               lastAnimFrameMs < roundPhaseStartMs)) ||
                (conAnimating && (now - lastAnimFrameMs >= 500 ||
                                  lastAnimFrameMs < roundPhaseStartMs))) {
                lastAnimFrameMs = now;
                portENTER_CRITICAL(&gameMux);
                displayDirty = true;
                portEXIT_CRITICAL(&gameMux);
            }

            // Host-only: auto-advance when a timed phase expires.
            if (cdHost) {
                uint32_t elapsed = (now >= roundPhaseStartMs)
                                       ? now - roundPhaseStartMs : 0;
                CountdownRoundSubPhase nextSP = SP::None;
                uint32_t timeout = 0;

                if (sp == SP::Intro) {
                    timeout = 5000;
                    // Advance to picking sub-phase for the selected round type.
                    auto rt = static_cast<flip7::countdown::RoundType>(
                        snap.roundType);
                    if (rt == flip7::countdown::RoundType::Numbers)
                        nextSP = SP::NumPicking;
                    else if (rt == flip7::countdown::RoundType::Letters)
                        nextSP = SP::LetPicking;
                    else
                        nextSP = SP::ConReady;
                } else if (sp == SP::NumThinking) {
                    timeout = 30000;
                    nextSP  = SP::NumClaimEntry;
                } else if (sp == SP::LetThinking) {
                    timeout = 30000;
                    nextSP  = SP::LetClaimEntry;
                } else if (sp == SP::ConActive) {
                    timeout = 30000;
                    nextSP  = SP::ConResultNoWinner;
                }

                if (nextSP != SP::None && elapsed >= timeout) {
                    CountdownWireState advanced = snap;
                    if (advanceCountdownSubPhase(advanced, boardId, nextSP)) {
                        roundPhaseStartMs = now;
                        portENTER_CRITICAL(&gameMux);
                        if (sameCountdownWireState(countdownState, snap)) {
                            countdownState = advanced;
                            countdownPendingDelivery = CountdownPendingDelivery{
                                true, MessageType::CountdownState, advanced, 0};
                            displayDirty = true;
                        }
                        portEXIT_CRITICAL(&gameMux);
                    }
                }

                // Auto-advance result sub-phases → BetweenRounds (with host handoff)
                bool isResultPhase = (sp == SP::NumResult   ||
                                      sp == SP::LetResult   ||
                                      sp == SP::ConResult   ||
                                      sp == SP::ConResultNoWinner);
                if (isResultPhase && elapsed >= 3000) {
                    // 1. Finalize the round in the engine and get the result.
                    std::optional<flip7::countdown::RoundResult> result;
                    portENTER_CRITICAL(&gameMux);
                    if (countdownEngine.activeRound()) {
                        std::vector<uint32_t> players = {snap.hostBoardId,
                                                          snap.guestBoardId};
                        result = countdownEngine.activeRound()->tryFinalize(players);
                        if (result) {
                            countdownEngine.recordRoundResult(*result);
                        }
                    }
                    portEXIT_CRITICAL(&gameMux);

                    // 2. Detect host change; build updated wire state.
                    uint32_t newHostId{};
                    uint32_t newChooserId{};
                    int32_t newHostScore{};
                    int32_t newGuestScore{};
                    portENTER_CRITICAL(&gameMux);
                    newHostId     = countdownEngine.hostBoardId();
                    newChooserId  = countdownEngine.chooserBoardId();
                    newHostScore  = countdownEngine.hostPlayer().score;
                    newGuestScore = countdownEngine.guestPlayer().score;
                    portEXIT_CRITICAL(&gameMux);

                    bool hostChanged = (newHostId != snap.hostBoardId);

                    CountdownWireState updated = snap;
                    ++updated.revision;
                    ++updated.roundNumber;
                    updated.chooserBoardId = newChooserId;
                    updated.hostTerm      += hostChanged ? 1 : 0;

                    // Swap host/guest scores and IDs if leadership transferred.
                    if (hostChanged) {
                        updated.hostBoardId  = newHostId;
                        updated.guestBoardId = snap.hostBoardId;
                        updated.hostScore    = newGuestScore;
                        updated.guestScore   = newHostScore;
                        updated.roundSubPhase =
                            static_cast<uint8_t>(SP::HostTransfer);
                    } else {
                        updated.hostScore    = newHostScore;
                        updated.guestScore   = newGuestScore;
                        updated.roundSubPhase = 0;
                        updated.phase        = CountdownWirePhase::BetweenRounds;
                    }

                    roundPhaseStartMs = now;
                    // Reset per-round UI
                    numUi = NumUiState{};
                    letUi = LetUiState{};
                    conUi = ConUiState{};

                    portENTER_CRITICAL(&gameMux);
                    if (sameCountdownWireState(countdownState, snap)) {
                        countdownState = updated;
                        countdownPendingDelivery = CountdownPendingDelivery{
                            true, MessageType::CountdownState, updated, 0};
                        displayDirty = true;
                    }
                    portEXIT_CRITICAL(&gameMux);
                }

                // HostTransfer: after 2s animation, commit and go to BetweenRounds
                if (sp == SP::HostTransfer && elapsed >= 2000) {
                    CountdownWireState committed = snap;
                    committed.roundSubPhase = 0;
                    committed.phase         = CountdownWirePhase::BetweenRounds;
                    ++committed.revision;
                    roundPhaseStartMs = now;
                    portENTER_CRITICAL(&gameMux);
                    if (sameCountdownWireState(countdownState, snap)) {
                        countdownState = committed;
                        countdownPendingDelivery = CountdownPendingDelivery{
                            true, MessageType::CountdownState, committed, 0};
                        displayDirty = true;
                    }
                    portEXIT_CRITICAL(&gameMux);
                }

                // NumClaimEntry: if both claims are in (host side), auto-advance
                if (sp == SP::NumClaimEntry && cdHost) {
                    portENTER_CRITICAL(&gameMux);
                    auto* nr = countdownEngine.numbersRound();
                    bool both = nr && nr->bothClaimsSubmitted();
                    portEXIT_CRITICAL(&gameMux);
                    if (both) {
                        CountdownWireState advanced = snap;
                        if (advanceCountdownSubPhase(advanced, boardId,
                                SP::NumClaimReveal)) {
                            roundPhaseStartMs = now;
                            portENTER_CRITICAL(&gameMux);
                            if (sameCountdownWireState(countdownState, snap)) {
                                countdownState = advanced;
                                countdownPendingDelivery = CountdownPendingDelivery{
                                    true, MessageType::CountdownState, advanced, 0};
                                displayDirty = true;
                            }
                            portEXIT_CRITICAL(&gameMux);
                        }
                    }
                }

                // NumClaimReveal: after 2s, advance to presentation
                if (sp == SP::NumClaimReveal && elapsed >= 2000) {
                    CountdownWireState advanced = snap;
                    if (advanceCountdownSubPhase(advanced, boardId,
                            SP::NumPresentPlayerA)) {
                        roundPhaseStartMs = now;
                        portENTER_CRITICAL(&gameMux);
                        if (sameCountdownWireState(countdownState, snap)) {
                            countdownState = advanced;
                            countdownPendingDelivery = CountdownPendingDelivery{
                                true, MessageType::CountdownState, advanced, 0};
                            displayDirty = true;
                        }
                        portEXIT_CRITICAL(&gameMux);
                    }
                }

                // LetClaimEntry: both claims in → reveal
                if (sp == SP::LetClaimEntry && cdHost) {
                    portENTER_CRITICAL(&gameMux);
                    auto* lr = countdownEngine.lettersRound();
                    bool both = lr && lr->bothClaimsSubmitted();
                    portEXIT_CRITICAL(&gameMux);
                    if (both) {
                        CountdownWireState advanced = snap;
                        if (advanceCountdownSubPhase(advanced, boardId,
                                SP::LetClaimReveal)) {
                            roundPhaseStartMs = now;
                            portENTER_CRITICAL(&gameMux);
                            if (sameCountdownWireState(countdownState, snap)) {
                                countdownState = advanced;
                                countdownPendingDelivery = CountdownPendingDelivery{
                                    true, MessageType::CountdownState, advanced, 0};
                                displayDirty = true;
                            }
                            portEXIT_CRITICAL(&gameMux);
                        }
                    }
                }

                // LetClaimReveal: after 2s → first presentation
                if (sp == SP::LetClaimReveal && elapsed >= 2000) {
                    // Determine presenter order from claims
                    portENTER_CRITICAL(&gameMux);
                    auto* lr = countdownEngine.lettersRound();
                    uint8_t hc = lr ? lr->claimFor(snap.hostBoardId) : 0;
                    uint8_t gc = lr ? lr->claimFor(snap.guestBoardId) : 0;
                    portEXIT_CRITICAL(&gameMux);
                    letUi.presenterIsHost =
                        (hc >= gc);  // ties: host presents first
                    CountdownWireState advanced = snap;
                    if (advanceCountdownSubPhase(advanced, boardId,
                            SP::LetPresentPlayerA)) {
                        roundPhaseStartMs = now;
                        portENTER_CRITICAL(&gameMux);
                        if (sameCountdownWireState(countdownState, snap)) {
                            countdownState = advanced;
                            countdownPendingDelivery = CountdownPendingDelivery{
                                true, MessageType::CountdownState, advanced, 0};
                            displayDirty = true;
                        }
                        portEXIT_CRITICAL(&gameMux);
                    }
                }

                // ConReady: after 2s → ConActive
                if (sp == SP::ConReady && elapsed >= 2000) {
                    // Start conundrum round in engine
                    portENTER_CRITICAL(&gameMux);
                    auto random = flip7::countdown::makeRoundRandom(
                        snap.gameId, snap.roundNumber);
                    countdownEngine.startNextRound(
                        flip7::countdown::RoundType::Conundrum, random);
                    // Reset conundrum circle order
                    for (uint8_t ci = 0; ci < 9; ++ci) conUi.circleOrder[ci] = ci;
                    portEXIT_CRITICAL(&gameMux);

                    CountdownWireState advanced = snap;
                    if (advanceCountdownSubPhase(advanced, boardId,
                            SP::ConActive)) {
                        roundPhaseStartMs = now;
                        portENTER_CRITICAL(&gameMux);
                        if (sameCountdownWireState(countdownState, snap)) {
                            countdownState = advanced;
                            countdownPendingDelivery = CountdownPendingDelivery{
                                true, MessageType::CountdownState, advanced, 0};
                            displayDirty = true;
                        }
                        portEXIT_CRITICAL(&gameMux);
                    }
                }
            }
        }
    }

    handleTouch();

    const bool online = peerOnline();
    if (online != lastOnline) {
        lastOnline = online;
        portENTER_CRITICAL(&gameMux);
        displayDirty = true;
        portEXIT_CRITICAL(&gameMux);
    }

    bool shouldRender = false;
    bool shouldLogRemote = false;
    bool shouldLogAck = false;
    bool shouldLogFull = false;
    uint32_t revision = 0;
    uint32_t sender = 0;
    uint32_t ackGameId = 0;
    uint32_t ackRevision = 0;
    uint32_t fullGameId = 0;
    uint32_t fullRevision = 0;
    portENTER_CRITICAL(&gameMux);
    if (screenMode == ScreenMode::Complete && completedAtMs != 0 &&
        now - completedAtMs >= kCompleteDisplayMs &&
        canReturnCompletedGameHome(activeGame, ActiveGameKind::Puzzle,
                                   pendingDelivery.active)) {
        screenMode = ScreenMode::Home;
        activeGame.kind = ActiveGameKind::Home;
        completedAtMs = 0;
        sendFullStateSoon = false;
        requestStateSoon = false;
        reconciliationPending = false;
        displayDirty = true;
    }
    shouldRender = displayDirty;
    displayDirty = false;
    shouldLogRemote = remoteLogPending;
    remoteLogPending = false;
    shouldLogAck = ackLogPending;
    ackLogPending = false;
    shouldLogFull = fullLogPending;
    fullLogPending = false;
    revision = remoteLogRevision;
    sender = remoteLogSender;
    ackGameId = ackLogGameId;
    ackRevision = ackLogRevision;
    fullGameId = fullLogGameId;
    fullRevision = fullLogRevision;
    portEXIT_CRITICAL(&gameMux);

    if (shouldRender) {
        renderScreen();
    }
    if (shouldLogRemote) {
        Serial.printf("REMOTE applied sender=%08lX revision=%lu\n",
                      static_cast<unsigned long>(sender),
                      static_cast<unsigned long>(revision));
    }
    if (shouldLogAck) {
        Serial.printf("ACK received game=%lu revision=%lu\n",
                      static_cast<unsigned long>(ackGameId),
                      static_cast<unsigned long>(ackRevision));
    }
    if (shouldLogFull) {
        Serial.printf("FULL applied game=%lu revision=%lu\n",
                      static_cast<unsigned long>(fullGameId),
                      static_cast<unsigned long>(fullRevision));
    }
    delay(10);
}
