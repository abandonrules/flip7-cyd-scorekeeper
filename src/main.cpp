#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <esp_now.h>

#include <cstring>

#include "protocol.h"

namespace {
constexpr uint8_t kBacklightPin = 21;
constexpr uint32_t kHeartbeatIntervalMs = 1000;
constexpr uint32_t kDisplayIntervalMs = 250;
constexpr uint32_t kPeerTimeoutMs = 3000;
constexpr uint8_t kBroadcastAddress[] = {0xff, 0xff, 0xff,
                                         0xff, 0xff, 0xff};

TFT_eSPI display;
portMUX_TYPE linkMux = portMUX_INITIALIZER_UNLOCKED;

struct LinkState {
    uint32_t sent;
    uint32_t sendFailures;
    uint32_t received;
    uint32_t lastReceivedMs;
    uint32_t peerSequence;
    uint8_t peerAddress[6];
};

LinkState linkState{};
uint32_t boardId = 0;
uint32_t nextSequence = 1;
uint32_t lastHeartbeatMs = 0;
uint32_t lastDisplayMs = 0;
bool espNowReady = false;

void formatAddress(const uint8_t* address, char* output, size_t size) {
    snprintf(output, size, "%02X:%02X:%02X:%02X:%02X:%02X", address[0],
             address[1], address[2], address[3], address[4], address[5]);
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

void onPacketReceived(const uint8_t* address, const uint8_t* data, int length) {
    if (length != sizeof(HeartbeatPacket)) {
        return;
    }

    HeartbeatPacket packet{};
    memcpy(&packet, data, sizeof(packet));
    if (packet.magic != kProtocolMagic ||
        packet.version != kProtocolVersion ||
        packet.type != MessageType::Heartbeat || packet.senderId == boardId) {
        return;
    }

    portENTER_CRITICAL(&linkMux);
    ++linkState.received;
    linkState.lastReceivedMs = millis();
    linkState.peerSequence = packet.sequence;
    memcpy(linkState.peerAddress, address, sizeof(linkState.peerAddress));
    const uint32_t received = linkState.received;
    portEXIT_CRITICAL(&linkMux);

    char peer[18];
    formatAddress(address, peer, sizeof(peer));
    Serial.printf("RX peer=%s sequence=%lu total=%lu\n", peer,
                  static_cast<unsigned long>(packet.sequence),
                  static_cast<unsigned long>(received));
}

bool startEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW initialization failed");
        return false;
    }

    esp_now_register_send_cb(onPacketSent);
    esp_now_register_recv_cb(onPacketReceived);

    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, kBroadcastAddress, sizeof(kBroadcastAddress));
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("ESP-NOW broadcast peer registration failed");
        return false;
    }

    return true;
}

void sendHeartbeat() {
    HeartbeatPacket packet{
        kProtocolMagic,
        kProtocolVersion,
        MessageType::Heartbeat,
        0,
        boardId,
        nextSequence++,
        millis(),
    };

    const esp_err_t result =
        esp_now_send(kBroadcastAddress, reinterpret_cast<uint8_t*>(&packet),
                     sizeof(packet));
    if (result != ESP_OK) {
        portENTER_CRITICAL(&linkMux);
        ++linkState.sendFailures;
        portEXIT_CRITICAL(&linkMux);
        Serial.printf("TX queue failed: %d\n", result);
    }
}

void renderLinkState() {
    LinkState snapshot{};
    portENTER_CRITICAL(&linkMux);
    snapshot = linkState;
    portEXIT_CRITICAL(&linkMux);

    const bool online = snapshot.received > 0 &&
                        millis() - snapshot.lastReceivedMs <= kPeerTimeoutMs;
    char line[64];
    char peer[18] = "--:--:--:--:--:--";
    if (snapshot.received > 0) {
        formatAddress(snapshot.peerAddress, peer, sizeof(peer));
    }

    display.fillRect(0, 52, display.width(), display.height() - 52, TFT_NAVY);
    display.setTextDatum(TL_DATUM);

    display.setTextColor(online ? TFT_GREEN : TFT_ORANGE, TFT_NAVY);
    display.drawString(online ? "Peer: ONLINE" : "Peer: WAITING", 12, 62, 4);

    display.setTextColor(TFT_WHITE, TFT_NAVY);
    snprintf(line, sizeof(line), "This board: %s", WiFi.macAddress().c_str());
    display.drawString(line, 12, 102, 2);
    snprintf(line, sizeof(line), "Peer:       %s", peer);
    display.drawString(line, 12, 124, 2);
    snprintf(line, sizeof(line), "TX ok: %-8lu  failed: %lu",
             static_cast<unsigned long>(snapshot.sent),
             static_cast<unsigned long>(snapshot.sendFailures));
    display.drawString(line, 12, 150, 2);
    snprintf(line, sizeof(line), "RX: %-11lu  sequence: %lu",
             static_cast<unsigned long>(snapshot.received),
             static_cast<unsigned long>(snapshot.peerSequence));
    display.drawString(line, 12, 172, 2);

    display.setTextColor(TFT_LIGHTGREY, TFT_NAVY);
    display.drawString("ESP-NOW broadcast heartbeat every second", 12, 210, 1);
}
}

void setup() {
    Serial.begin(115200);

    pinMode(kBacklightPin, OUTPUT);
    digitalWrite(kBacklightPin, HIGH);

    display.init();
    display.setRotation(1);
    display.fillScreen(TFT_NAVY);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    display.setTextDatum(MC_DATUM);
    display.drawString("Flip7 CYD Link", display.width() / 2, 24, 4);

    boardId = static_cast<uint32_t>(ESP.getEfuseMac());
    espNowReady = startEspNow();
    Serial.printf("CYD link ready: mac=%s id=%08lX esp-now=%s\n",
                  WiFi.macAddress().c_str(),
                  static_cast<unsigned long>(boardId),
                  espNowReady ? "ready" : "failed");
    renderLinkState();
}

void loop() {
    const uint32_t now = millis();
    if (espNowReady && now - lastHeartbeatMs >= kHeartbeatIntervalMs) {
        lastHeartbeatMs = now;
        sendHeartbeat();
    }
    if (now - lastDisplayMs >= kDisplayIntervalMs) {
        lastDisplayMs = now;
        renderLinkState();
    }
    delay(10);
}
