#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include <esp_now.h>

#include <algorithm>
#include <cstring>

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
constexpr uint32_t kPeerTimeoutMs = 3000;
constexpr uint8_t kBroadcastAddress[] = {0xff, 0xff, 0xff,
                                         0xff, 0xff, 0xff};
constexpr int16_t kBoardX = 11;
constexpr int16_t kBoardY = 43;
constexpr int16_t kTileWidth = 70;
constexpr int16_t kTileHeight = 50;
constexpr int16_t kTileGap = 6;

TFT_eSPI display;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(kTouchCsPin, kTouchIrqPin);
portMUX_TYPE linkMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE gameMux = portMUX_INITIALIZER_UNLOCKED;

struct LinkState {
    uint32_t sent;
    uint32_t sendFailures;
    uint32_t received;
    uint32_t lastReceivedMs;
    uint32_t peerSequence;
    uint32_t peerBoardId;
    uint8_t peerAddress[6];
};

LinkState linkState{};
PuzzleState puzzleState{};
uint32_t boardId = 0;
uint32_t gamePeerBoardId = 0;
uint32_t nextSequence = 1;
uint32_t lastHeartbeatMs = 0;
uint32_t remoteLogRevision = 0;
uint32_t remoteLogSender = 0;
bool espNowReady = false;
bool gameReady = false;
bool displayDirty = true;
bool remoteLogPending = false;
bool touchWasDown = false;

void formatAddress(const uint8_t* address, char* output, size_t size) {
    snprintf(output, size, "%02X:%02X:%02X:%02X:%02X:%02X", address[0],
             address[1], address[2], address[3], address[4], address[5]);
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
        case 1:  // alpha
            display.drawCircle(cx - 3, cy + 3, 10, color);
            display.drawCircle(cx - 3, cy + 3, 11, color);
            drawThickLine(cx + 7, top + 3, cx + 7, bottom, color);
            drawThickLine(cx + 7, cy + 4, right, cy - 8, color);
            break;
        case 2:  // beta
            drawThickLine(left + 5, top, left + 5, bottom, color);
            display.drawCircle(cx - 1, cy - 8, 9, color);
            display.drawCircle(cx - 1, cy + 9, 10, color);
            break;
        case 3:  // gamma
            drawThickLine(left, top, right, top, color);
            drawThickLine(cx, top, cx, bottom, color);
            break;
        case 4:  // delta
            drawThickLine(cx, top, left, bottom, color);
            drawThickLine(cx, top, right, bottom, color);
            drawThickLine(left, bottom, right, bottom, color);
            break;
        case 5:  // epsilon
            drawThickLine(right, top, left, top, color);
            drawThickLine(left, top, left, bottom, color);
            drawThickLine(left, cy, cx + 8, cy, color);
            drawThickLine(left, bottom, right, bottom, color);
            break;
        case 6:  // zeta
            drawThickLine(left, top, right, top, color);
            drawThickLine(right, top, left, bottom, color);
            drawThickLine(left, bottom, right, bottom, color);
            break;
        case 7:  // eta
            drawThickLine(left + 3, top, left + 3, bottom, color);
            drawThickLine(right - 3, top, right - 3, bottom, color);
            drawThickLine(left + 3, cy, right - 3, cy, color);
            break;
        case 8:  // theta
            display.drawEllipse(cx, cy, 13, 16, color);
            display.drawEllipse(cx, cy, 12, 15, color);
            drawThickLine(left + 3, cy, right - 3, cy, color);
            break;
        case 9:  // iota
            drawThickLine(cx, top, cx, bottom, color);
            drawThickLine(cx - 7, top, cx + 7, top, color);
            drawThickLine(cx - 7, bottom, cx + 7, bottom, color);
            break;
        case 10:  // kappa
            drawThickLine(left + 3, top, left + 3, bottom, color);
            drawThickLine(left + 3, cy, right, top, color);
            drawThickLine(left + 3, cy, right, bottom, color);
            break;
        case 11:  // lambda
            drawThickLine(cx, top, left, bottom, color);
            drawThickLine(cx, top, right, bottom, color);
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

void renderPuzzle() {
    PuzzleState game{};
    bool ready = false;
    portENTER_CRITICAL(&gameMux);
    game = puzzleState;
    ready = gameReady;
    portEXIT_CRITICAL(&gameMux);

    LinkState link{};
    portENTER_CRITICAL(&linkMux);
    link = linkState;
    portEXIT_CRITICAL(&linkMux);
    const bool online = link.received > 0 &&
                        millis() - link.lastReceivedMs <= kPeerTimeoutMs;

    display.fillScreen(TFT_NAVY);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    display.drawString("GREEK SLIDE", display.width() / 2, 13, 4);
    display.setTextColor(online ? TFT_GREEN : TFT_ORANGE, TFT_NAVY);
    display.drawString(online ? "LINKED" : "WAITING", 286, 31, 1);

    for (uint8_t position = 0; position < kPuzzleTileCount; ++position) {
        const int16_t column = position % kPuzzleColumns;
        const int16_t row = position / kPuzzleColumns;
        const int16_t x = kBoardX + column * (kTileWidth + kTileGap);
        const int16_t y = kBoardY + row * (kTileHeight + kTileGap);
        const uint8_t tile = ready ? game.tiles[position] : 0;
        if (tile == 0) {
            display.drawRoundRect(x, y, kTileWidth, kTileHeight, 7,
                                  TFT_DARKGREY);
            display.drawRoundRect(x + 1, y + 1, kTileWidth - 2,
                                  kTileHeight - 2, 6, TFT_DARKGREY);
            continue;
        }
        const uint16_t background = tileColor(tile);
        display.fillRoundRect(x, y, kTileWidth, kTileHeight, 7, background);
        display.drawRoundRect(x, y, kTileWidth, kTileHeight, 7, TFT_WHITE);
        drawGreekSymbol(tile, x + kTileWidth / 2, y + kTileHeight / 2,
                        TFT_WHITE);
    }

    display.fillRect(0, 211, display.width(), 29, TFT_NAVY);
    display.setTextDatum(MC_DATUM);
    if (!ready) {
        display.setTextColor(TFT_ORANGE, TFT_NAVY);
        display.drawString("WAITING FOR PEER", display.width() / 2, 225, 2);
    } else if (game.turnBoardId == boardId) {
        display.setTextColor(TFT_GREEN, TFT_NAVY);
        display.drawString("YOUR TURN - TAP NEXT TO EMPTY",
                           display.width() / 2, 225, 2);
    } else {
        display.setTextColor(TFT_CYAN, TFT_NAVY);
        display.drawString("PEER TURN", display.width() / 2, 225, 2);
    }
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

void rememberPeer(const uint8_t* address, uint32_t senderId,
                  uint32_t sequence) {
    portENTER_CRITICAL(&linkMux);
    ++linkState.received;
    linkState.lastReceivedMs = millis();
    linkState.peerSequence = sequence;
    linkState.peerBoardId = senderId;
    memcpy(linkState.peerAddress, address, sizeof(linkState.peerAddress));
    portEXIT_CRITICAL(&linkMux);
}

void onPacketReceived(const uint8_t* address, const uint8_t* data, int length) {
    if (length == sizeof(HeartbeatPacket)) {
        HeartbeatPacket packet{};
        memcpy(&packet, data, sizeof(packet));
        if (packet.magic != kProtocolMagic ||
            packet.version != kProtocolVersion ||
            packet.type != MessageType::Heartbeat ||
            packet.senderId == boardId) {
            return;
        }
        rememberPeer(address, packet.senderId, packet.sequence);
        return;
    }

    if (length != sizeof(PuzzleStatePacket)) {
        return;
    }
    PuzzleStatePacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (packet.magic != kProtocolMagic ||
        packet.version != kProtocolVersion ||
        packet.type != MessageType::PuzzleState || packet.senderId == boardId) {
        return;
    }
    rememberPeer(address, packet.senderId, packet.sequence);

    portENTER_CRITICAL(&gameMux);
    if (gameReady && packet.senderId == gamePeerBoardId &&
        isValidRemoteTransition(puzzleState, packet.state, packet.senderId,
                                boardId)) {
        puzzleState = packet.state;
        displayDirty = true;
        remoteLogRevision = puzzleState.revision;
        remoteLogSender = packet.senderId;
        remoteLogPending = true;
    }
    portEXIT_CRITICAL(&gameMux);
}

bool startEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if (esp_now_init() != ESP_OK) {
        return false;
    }
    esp_now_register_send_cb(onPacketSent);
    esp_now_register_recv_cb(onPacketReceived);

    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, kBroadcastAddress, sizeof(kBroadcastAddress));
    peer.channel = 0;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

void sendHeartbeat() {
    HeartbeatPacket packet{
        kProtocolMagic, kProtocolVersion, MessageType::Heartbeat, 0,
        boardId,       nextSequence++,  millis(),
    };
    if (esp_now_send(kBroadcastAddress,
                     reinterpret_cast<uint8_t*>(&packet),
                     sizeof(packet)) != ESP_OK) {
        portENTER_CRITICAL(&linkMux);
        ++linkState.sendFailures;
        portEXIT_CRITICAL(&linkMux);
    }
}

void sendPuzzleState(const PuzzleState& state) {
    PuzzleStatePacket packet{
        kProtocolMagic, kProtocolVersion, MessageType::PuzzleState, 0,
        boardId,       nextSequence++,  state,
    };
    const esp_err_t result = esp_now_send(
        kBroadcastAddress, reinterpret_cast<uint8_t*>(&packet), sizeof(packet));
    Serial.printf("TX puzzle revision=%lu result=%d\n",
                  static_cast<unsigned long>(state.revision), result);
}

void initializeGameWhenLinked() {
    uint32_t peerId = 0;
    portENTER_CRITICAL(&linkMux);
    peerId = linkState.peerBoardId;
    portEXIT_CRITICAL(&linkMux);
    if (peerId == 0) {
        return;
    }

    portENTER_CRITICAL(&gameMux);
    if (!gameReady) {
        gamePeerBoardId = peerId;
        puzzleState = makeInitialPuzzle(std::min(boardId, peerId));
        gameReady = true;
        displayDirty = true;
    }
    const uint32_t firstTurn = puzzleState.turnBoardId;
    portEXIT_CRITICAL(&gameMux);

    static bool logged = false;
    if (!logged) {
        logged = true;
        Serial.printf("PUZZLE ready peer=%08lX first-turn=%08lX role=%s\n",
                      static_cast<unsigned long>(peerId),
                      static_cast<unsigned long>(firstTurn),
                      firstTurn == boardId ? "first" : "second");
    }
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

int8_t puzzlePositionAt(int16_t x, int16_t y) {
    if (x < kBoardX || y < kBoardY) {
        return -1;
    }
    const int16_t column = (x - kBoardX) / (kTileWidth + kTileGap);
    const int16_t row = (y - kBoardY) / (kTileHeight + kTileGap);
    if (column < 0 || column >= kPuzzleColumns || row < 0 ||
        row >= kPuzzleRows) {
        return -1;
    }
    const int16_t localX = (x - kBoardX) % (kTileWidth + kTileGap);
    const int16_t localY = (y - kBoardY) % (kTileHeight + kTileGap);
    if (localX >= kTileWidth || localY >= kTileHeight) {
        return -1;
    }
    return row * kPuzzleColumns + column;
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

    const int8_t position = puzzlePositionAt(x, y);
    Serial.printf("TOUCH raw=%d,%d screen=%d,%d position=%d\n", point.x,
                  point.y, x, y, position);
    if (position < 0) {
        return;
    }

    PuzzleState moved{};
    bool accepted = false;
    uint8_t tile = 0;
    portENTER_CRITICAL(&gameMux);
    if (gameReady) {
        tile = puzzleState.tiles[position];
        accepted = tryPuzzleMove(puzzleState, position, boardId,
                                 gamePeerBoardId);
        if (accepted) {
            moved = puzzleState;
            displayDirty = true;
        }
    }
    portEXIT_CRITICAL(&gameMux);

    if (!accepted) {
        Serial.printf("MOVE ignored position=%d\n", position);
        return;
    }
    Serial.printf("LOCAL move symbol=%u revision=%lu next=%08lX\n", tile,
                  static_cast<unsigned long>(moved.revision),
                  static_cast<unsigned long>(moved.turnBoardId));
    sendPuzzleState(moved);
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
    espNowReady = startEspNow();
    Serial.printf("GREEK SLIDE ready mac=%s id=%08lX esp-now=%s\n",
                  WiFi.macAddress().c_str(),
                  static_cast<unsigned long>(boardId),
                  espNowReady ? "ready" : "failed");
    renderPuzzle();
}

void loop() {
    const uint32_t now = millis();
    if (espNowReady && now - lastHeartbeatMs >= kHeartbeatIntervalMs) {
        lastHeartbeatMs = now;
        sendHeartbeat();
    }
    initializeGameWhenLinked();
    handleTouch();

    bool shouldRender = false;
    bool shouldLogRemote = false;
    uint32_t revision = 0;
    uint32_t sender = 0;
    portENTER_CRITICAL(&gameMux);
    shouldRender = displayDirty;
    displayDirty = false;
    shouldLogRemote = remoteLogPending;
    remoteLogPending = false;
    revision = remoteLogRevision;
    sender = remoteLogSender;
    portEXIT_CRITICAL(&gameMux);
    if (shouldRender) {
        renderPuzzle();
    }
    if (shouldLogRemote) {
        Serial.printf("REMOTE applied sender=%08lX revision=%lu\n",
                      static_cast<unsigned long>(sender),
                      static_cast<unsigned long>(revision));
    }
    delay(10);
}
