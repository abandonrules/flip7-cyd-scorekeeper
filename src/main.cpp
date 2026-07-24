#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include <esp_now.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>

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

TFT_eSPI display;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(kTouchCsPin, kTouchIrqPin);
portMUX_TYPE linkMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE gameMux = portMUX_INITIALIZER_UNLOCKED;

enum class ScreenMode : uint8_t { Home, Puzzle, Complete };

struct LinkState {
    uint32_t sent;
    uint32_t sendFailures;
    uint32_t received;
    uint32_t lastReceivedMs;
    uint32_t peerBoardId;
    uint32_t peerSessionId;
    uint32_t retiredSessions[kRetiredSessionCapacity];
    size_t retiredSessionCount;
    uint32_t peerSequences[7];
    bool sequenceSeen[7];
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

LinkState linkState{};
PuzzleState puzzleState{};
PendingDelivery pendingDelivery{};
PendingAck pendingAck{};
ScreenMode screenMode = ScreenMode::Home;
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
bool touchWasDown = false;
bool lastOnline = false;

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
    if (index >= 7) {
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
    display.drawString("SLIDE PUZZLES", display.width() / 2, 28, 4);
    renderLinkBadge(online);

    if (deliveryPending) {
        display.setTextColor(TFT_YELLOW, TFT_NAVY);
        display.drawString("RETURNING BOTH BOARDS HOME...",
                           display.width() / 2, 123, 2);
        return;
    }
    display.setTextColor(TFT_CYAN, TFT_NAVY);
    display.drawString("Choose a synchronized puzzle", display.width() / 2,
                       61, 2);
    if (localIsHost() && online) {
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

void renderScreen() {
    PuzzleState game{};
    ScreenMode mode;
    bool ready;
    bool deliveryPending;
    portENTER_CRITICAL(&gameMux);
    game = puzzleState;
    mode = screenMode;
    ready = stateReady;
    deliveryPending = pendingDelivery.active;
    portEXIT_CRITICAL(&gameMux);

    const bool online = peerOnline();
    if (mode == ScreenMode::Home || !ready) {
        renderHome(online, deliveryPending);
    } else if (mode == ScreenMode::Complete) {
        renderComplete(online, game);
    } else {
        renderPuzzle(online, game, deliveryPending);
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
    if (stateReady) {
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
    if (header.type == MessageType::PuzzleState ||
        header.type == MessageType::FullState) {
        processStatePacket(address, data, length, header.type);
    } else if (header.type == MessageType::PuzzleAck) {
        processAckPacket(address, data, length);
    } else if (header.type == MessageType::RequestState) {
        processStateRequest(address, data, length);
    }
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

void sendState(MessageType type, const PuzzleState& state) {
    PuzzleStatePacket packet{makeHeader(type), state};
    const esp_err_t result = esp_now_send(
        expectedPeerAddress, reinterpret_cast<uint8_t*>(&packet), sizeof(packet));
    Serial.printf("TX %s game=%lu revision=%lu result=%d\n",
                  type == MessageType::FullState ? "full" : "move",
                  static_cast<unsigned long>(state.gameId),
                  static_cast<unsigned long>(state.revision), result);
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
    if (pendingDelivery.active || !isSupportedPuzzleSpec(spec)) {
        portEXIT_CRITICAL(&gameMux);
        return;
    }
    const uint32_t nextGameId = stateReady ? puzzleState.gameId + 1 : 1;
    started = makeScrambledPuzzle(std::min(boardId, gamePeerBoardId),
                                  nextGameId, spec);
    puzzleState = started;
    stateReady = true;
    screenMode = ScreenMode::Puzzle;
    pendingDelivery =
        PendingDelivery{true, MessageType::FullState, started, 0};
    displayDirty = true;
    portEXIT_CRITICAL(&gameMux);
    Serial.printf("GAME started id=%lu\n",
                  static_cast<unsigned long>(started.gameId));
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
        }
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
    bool sendDelivery = false;
    bool sendAckNow = false;
    bool sendFullNow = false;
    bool requestNow = false;
    portENTER_CRITICAL(&gameMux);
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
    if (sendFullStateSoon && stateReady) {
        sendFullStateSoon = false;
        fullState = puzzleState;
        sendFullNow = true;
    }
    if (requestStateSoon ||
        (gamePeerBoardId != 0 && (!stateReady || reconciliationPending) &&
         now - lastStateRequestMs >= kStateRequestIntervalMs)) {
        requestStateSoon = false;
        lastStateRequestMs = now;
        requestNow = true;
    }
    portEXIT_CRITICAL(&gameMux);

    if (sendDelivery) {
        sendState(delivery.type, delivery.state);
    }
    if (sendAckNow) {
        sendAck(ack);
    }
    if (sendFullNow) {
        sendState(MessageType::FullState, fullState);
    }
    if (requestNow) {
        sendStateRequest();
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
    espNowReady = startEspNow();
    Serial.printf("GREEK SLIDE ready mac=%s id=%08lX esp-now=%s\n",
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
        now - completedAtMs >= kCompleteDisplayMs) {
        screenMode = ScreenMode::Home;
        completedAtMs = 0;
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
