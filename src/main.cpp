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

enum class ScreenMode : uint8_t { Home, Puzzle, Complete, Mastermind };

struct LinkState {
    uint32_t sent;
    uint32_t sendFailures;
    uint32_t received;
    uint32_t lastReceivedMs;
    uint32_t peerBoardId;
    uint32_t peerSessionId;
    uint32_t retiredSessions[kRetiredSessionCapacity];
    size_t retiredSessionCount;
    uint32_t peerSequences[11];
    bool sequenceSeen[11];
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

LinkState linkState{};
PuzzleState puzzleState{};
PendingDelivery pendingDelivery{};
PendingAck pendingAck{};
MastermindState mastermindState{};
MastermindPendingDelivery mastermindPendingDelivery{};
MastermindPendingAck mastermindPendingAck{};
MastermindCode draftCode{{1, 1, 1, 1}};
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
bool touchWasDown = false;
bool lastOnline = false;
uint8_t selectedPeg = 0;

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
    if (index >= 11) {
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
                           61, 2);

        // Slide puzzles row
        display.fillRoundRect(10, 82, 145, 89, 10, TFT_PURPLE);
        display.drawRoundRect(10, 82, 145, 89, 10, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_PURPLE);
        display.drawString("PLANETS", 82, 111, 4);
        display.drawString("8 PIECES / 3x3", 82, 147, 2);

        display.fillRoundRect(165, 82, 145, 89, 10, TFT_DARKGREEN);
        display.drawRoundRect(165, 82, 145, 89, 10, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
        display.drawString("GREEK", 237, 111, 4);
        display.drawString("11 PIECES / 4x3", 237, 147, 2);

        // Mastermind row
        display.fillRoundRect(45, 185, 230, 55, 10, TFT_MAROON);
        display.drawRoundRect(45, 185, 230, 55, 10, TFT_WHITE);
        display.setTextColor(TFT_WHITE, TFT_MAROON);
        display.drawString("MASTERMIND", display.width() / 2, 212, 4);
    } else {
        display.setTextColor(online ? TFT_LIGHTGREY : TFT_ORANGE, TFT_NAVY);
        display.drawString(online ? "WAITING FOR HOST" : "CONNECT PEER",
                           display.width() / 2, 125, 2);
    }
    display.setTextColor(TFT_LIGHTGREY, TFT_NAVY);
    display.drawString("Locked pieces lose their background",
                       display.width() / 2, 207, 2);
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

void renderScreen() {
    PuzzleState game{};
    MastermindState mastermind{};
    ScreenMode mode;
    bool ready;
    bool mastermindReady;
    bool deliveryPending;
    bool mastermindDeliveryPending;
    portENTER_CRITICAL(&gameMux);
    game = puzzleState;
    mastermind = mastermindState;
    mode = screenMode;
    ready = stateReady;
    mastermindReady = mastermindStateReady;
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

void prepareMastermindDraft() {
    draftCode = MastermindCode{{1, 1, 1, 1}};
    selectedPeg = 0;
}

void noteMastermindPhase(const MastermindState& state) {
    prepareMastermindDraft();
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
        activeGame = {puzzleState.gameId, ActiveGameKind::Puzzle};
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
    MastermindStateRequestPacket packet{
        makeHeader(MessageType::MastermindRequestState), gameId, revision,
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
    if (activeGame.kind != ActiveGameKind::Home) {
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
         (activeGame.kind != ActiveGameKind::Home ||
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
        if (localIsHost() && peerOnline() && !pending && y >= 82 && y < 171) {
            if (x >= 10 && x < 155) {
                startPuzzle({3, 3, PuzzleTheme::Planets});
            } else if (x >= 165 && x < 310) {
                startPuzzle({4, 3, PuzzleTheme::Greek});
            }
        } else if (localIsHost() && peerOnline() && x >= 45 && x < 275) {
            if (y >= 185 && y < 240) {
                startMastermind();
            }
        }
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
    bool sendDelivery = false;
    bool sendAckNow = false;
    bool sendFullNow = false;
    bool requestNow = false;
    bool sendMastermindDeliveryNow = false;
    bool sendMastermindAckNow = false;
    bool sendMastermindFullNow = false;
    bool requestMastermindNow = false;
    portENTER_CRITICAL(&gameMux);
    const bool puzzleActive = activeGame.kind == ActiveGameKind::Puzzle;
    const bool mastermindActive =
        activeGame.kind == ActiveGameKind::Mastermind;
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
    if (requestNow || requestMastermindNow) {
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
