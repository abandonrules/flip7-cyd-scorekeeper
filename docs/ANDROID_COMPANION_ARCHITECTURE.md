# Flip7 CYD Scorekeeper - Android Companion Architecture

**Project**: flip7-cyd-scorekeeper  
**Repository**: abandonrules/flip7-cyd-scorekeeper  
**Status**: Proposed Architecture  
**Primary Platforms**: ESP32-2432S028 CYD boards and Android  
**Primary Languages**: C++, Kotlin  
**Communication**: ESP-NOW and Bluetooth Low Energy

---

## 1. Executive Summary

The Flip7 CYD Scorekeeper currently consists of two ESP32-2432S028 Cheap Yellow Display boards that synchronize games over encrypted ESP-NOW. Both CYD boards have now been verified to support:
- Bluetooth Low Energy
- Bluetooth Classic
- Dual-mode Bluetooth
- Wi-Fi
- ESP-NOW
- Dual-core ESP32 execution

The proposed architecture adds an Android companion application without replacing the existing CYD-to-CYD ESP-NOW connection. The Android application will communicate with one or both CYD boards using Bluetooth Low Energy. The existing C++ game rules, state validation, packet serialization, reconciliation logic, and protocol definitions will be extracted into a shared platform-independent core.

The shared C++ core will be compiled directly into:
- The ESP32 firmware
- The Android APK through the Android NDK and JNI

This approach provides one authoritative implementation of the game protocol and game rules while allowing each platform to maintain its own user interface and communication code.

---

## 2. Goals

### 2.1 Primary Goals
- Allow an Android application to communicate with the CYD boards
- Preserve the existing ESP-NOW connection between the two CYDs
- Reuse the existing C++ game and synchronization logic
- Prevent the Android and ESP32 implementations from diverging
- Support Android as either a player or spectator
- Keep game logic independent of BLE and ESP-NOW
- Establish a reusable architecture for future synchronized games
- Preserve offline operation between the CYD boards

### 2.2 Secondary Goals
- Support match history and statistics
- Allow Android to display synchronized game state
- Support future persistence and cloud backup
- Allow future transports such as Wi-Fi, USB, or WebSocket
- Make protocol behavior testable on desktop, Android, and ESP32
- Keep Android-specific code primarily in Kotlin

### 2.3 Non-Goals
The first implementation will not:
- Replace ESP-NOW with Bluetooth
- Require internet access
- Require a cloud service
- Move the Android user interface into C++
- Expose raw C++ pointers or native structs to Kotlin
- Depend on Bluetooth Classic
- Support arbitrary untrusted CYD devices
- Guarantee resistance against modified firmware or physical device extraction
- Rewrite the shared core in Rust or Kotlin Multiplatform

---

## 3. Current System

The existing system contains two CYD boards connected through ESP-NOW.

```
┌──────────────────┐          ESP-NOW          ┌──────────────────┐
│                  │◀─────────────────────────▶│                  │
│    CYD Board A   │                           │    CYD Board B   │
│                  │                           │                  │
└──────────────────┘                           └──────────────────┘
```

The boards synchronize:
- Heartbeats
- Active game selection
- Puzzle state
- Mastermind state
- Revisions and epochs
- Exit state
- Recovery state
- Game-specific actions

The Android application does not currently have a compatible transport into this network.

---

## 4. Proposed System

One or both CYD boards will expose a BLE service that acts as a packet transport for the existing Flip7 protocol.

```
                           Bluetooth Low Energy
                     ┌────────────────────────────┐
                     │                            │
                     ▼                            │
             ┌───────────────┐                    │
             │               │                    │
             │ Android APK   │                    │
             │               │                    │
             └───────────────┘                    │
                                                  │
                                                  ▼
┌──────────────────┐          ESP-NOW          ┌──────────────────┐
│                  │◀─────────────────────────▶│                  │
│    CYD Board A   │                           │    CYD Board B   │
│                  │                           │                  │
└──────────────────┘                           └──────────────────┘
```

The Android application sends and receives the same logical protocol packets used by the CYD system. BLE and ESP-NOW remain transport mechanisms only. Neither transport owns the game rules.

---

## 5. Architectural Principles

### 5.1 One Authoritative Core
Game rules and protocol behavior must exist in one shared C++ implementation. The same source code will be compiled for both ESP32 and Android.

### 5.2 Transport Independence
Game logic must not directly call:
- ESP-NOW APIs
- BLE APIs
- Android APIs
- Arduino APIs
- LVGL APIs
- Wi-Fi APIs

Instead, transports deliver serialized packets to the shared core:
```cpp
handleIncomingPacket(packetData, packetLength);
```

### 5.3 Explicit Serialization
Native C++ structs must not be copied directly onto the wire.
**Unsafe example:**
```cpp
memcpy(buffer, &state, sizeof(state));
```

The protocol must explicitly define:
- Integer width
- Field order
- Byte order
- Packet version
- Packet length
- Packet type
- Validation rules
- Optional checksum or digest

**Example:**
```cpp
writeU32LE(output, state.gameId);
writeU32LE(output, state.revision);
writeU8(output, static_cast<uint8_t>(state.phase));
```

### 5.4 Narrow JNI Boundary
The JNI interface should move bytes and simple values rather than expose the complete C++ object model.
**Preferred JNI inputs and outputs:**
- ByteArray
- Boolean
- Int
- Long
- Small immutable result objects
- JSON or another UI projection where appropriate

### 5.5 Platform-Owned User Interfaces
- The ESP32 uses its existing display and touch interface
- Android uses native Android UI, preferably Kotlin with Jetpack Compose
- The shared C++ core does not render screens

---

## 6. Logical Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                         UI Layer                            │
├──────────────────────────────┬──────────────────────────────┤
│ ESP32 LVGL / Touch UI        │ Android Jetpack Compose UI  │
└──────────────────────────────┴──────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                        │
├─────────────────────────────────────────────────────────────┤
│ Start game                                                   │
│ Join game                                                    │
│ Submit move                                                  │
│ Submit Mastermind guess                                      │
│ Exit game                                                    │
│ Request snapshot                                             │
│ Observe game                                                 │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                     Shared C++ Core                         │
├─────────────────────────────────────────────────────────────┤
│ Game rules                                                   │
│ State validation                                             │
│ Packet serialization                                         │
│ Packet deserialization                                       │
│ Revision comparison                                          │
│ Epoch management                                             │
│ Reconciliation decisions                                     │
│ Digest calculation                                           │
│ Puzzle generation                                            │
│ Move validation                                              │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                     Transport Layer                         │
├──────────────────────────────┬──────────────────────────────┤
│ ESP-NOW                     │ Bluetooth Low Energy          │
│ CYD ↔ CYD                   │ Android ↔ CYD                 │
└──────────────────────────────┴──────────────────────────────┘
```

---

## 7. Shared C++ Core

The shared core should contain platform-neutral code only.

### 7.1 Suggested Modules
```
core/
├── include/
│   └── flip7/
│       ├── protocol.hpp
│       ├── packet.hpp
│       ├── serializer.hpp
│       ├── reconciliation.hpp
│       ├── active_game.hpp
│       ├── mastermind.hpp
│       ├── puzzle.hpp
│       ├── aquarium.hpp
│       └── result.hpp
├── src/
│   ├── packet.cpp
│   ├── serializer.cpp
│   ├── reconciliation.cpp
│   ├── mastermind.cpp
│   ├── puzzle.cpp
│   └── aquarium.cpp
└── tests/
    ├── protocol_test.cpp
    ├── serialization_test.cpp
    ├── reconciliation_test.cpp
    ├── mastermind_test.cpp
    └── puzzle_test.cpp
```

### 7.2 Responsibilities
The shared core will:
- Validate incoming packets
- Decode protocol messages
- Apply valid messages to local state
- Reject malformed or stale messages
- Generate outgoing packets
- Calculate packet digests
- Compare revisions and epochs
- Determine reconciliation actions
- Execute game actions
- Validate state invariants
- Produce safe display projections

### 7.3 Prohibited Dependencies
The shared core must not include:
```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <NimBLEDevice.h>
#include <jni.h>
#include <lvgl.h>
```
Platform wrappers may include these dependencies, but the core must not.

---

## 8. BLE Architecture

### 8.1 BLE Role
- The CYD acts as a BLE peripheral
- The Android application acts as a BLE central

### 8.2 BLE Service
The firmware will expose one custom Flip7 service.

**Suggested conceptual design:**
```
Flip7 Service
├── Command characteristic
├── Event characteristic
└── Device information characteristic
```

A minimal first version may use only two packet characteristics:

**Command Characteristic**
- Android writes packets to the CYD
- Supports write with response initially
- Used for actions, requests, and state updates

**Event Characteristic**
- CYD sends notifications to Android
- Used for snapshots, acknowledgments, errors, and game events

**Device Information Characteristic**
- Optional read-only characteristic containing:
  - Protocol version
  - Firmware version
  - Board identifier
  - Device role
  - Active game
  - Feature flags

### 8.3 Why Two Packet Characteristics
Using separate command and event characteristics makes direction and permissions clear while retaining a generic packet-oriented design.
```
Android ──write──▶ Command
Android ◀─notify── Event
```

### 8.4 Packet Fragmentation
BLE payload capacity may be smaller than some protocol messages. The transport layer must support:
- Negotiated MTU
- Packet length prefix
- Message identifier
- Fragment index
- Fragment count
- Reassembly timeout
- Duplicate fragment handling
- Maximum message size

The shared game core should receive only a fully reassembled packet.

```
BLE fragments
      ↓
BLE reassembly
      ↓
Complete Flip7 packet
      ↓
Shared C++ core
```

---

## 9. CYD Gateway Behavior

### 9.1 Preferred Initial Design
Both boards may advertise BLE, but the Android application connects to only one board at a time. The connected board becomes the Android gateway.

```
Android
   │
   │ BLE
   ▼
CYD A
   │
   │ ESP-NOW
   ▼
CYD B
```

### 9.2 Gateway Responsibilities
The BLE-connected CYD will:
- Receive packets from Android
- Validate them
- Apply valid actions locally
- Forward relevant state through ESP-NOW
- Forward synchronized state changes back to Android
- Prevent packet loops
- Track packet origin
- Avoid applying the same message twice

### 9.3 Packet Origin
Each message should identify its source.
**Possible origin types:**
```cpp
enum class PacketOrigin : uint8_t {
    Unknown = 0,
    CydHost = 1,
    CydGuest = 2,
    Android = 3,
};
```

Each packet should also include a unique message identifier or an origin-specific sequence number.

This prevents the following loop:
```
Android → CYD A → CYD B → CYD A → Android → CYD A
```

### 9.4 Gateway Failover
The first release does not require seamless gateway failover. A later release may allow Android to reconnect to the surviving CYD if the original gateway disconnects.

---

## 10. Android Architecture

### 10.1 Suggested Android Stack
- Kotlin
- Jetpack Compose
- Android BLE APIs
- Android NDK
- CMake
- JNI
- Kotlin coroutines
- StateFlow
- Repository pattern

### 10.2 Android Layers
```
Android UI
   ↓
ViewModel
   ↓
Game Repository
   ├── BLE Transport
   └── Native Core Adapter
           ↓
         JNI
           ↓
      Shared C++ Core
```

### 10.3 Android Responsibilities
The Android application will:
- Scan for Flip7 CYD devices
- Display discovered boards
- Connect to a selected board
- Discover the Flip7 BLE service
- Negotiate notifications
- Reassemble BLE fragments
- Pass complete packets to the native core
- Display validated state
- Create user actions
- Ask the native core to generate valid outgoing packets
- Send packets through BLE
- Manage Android lifecycle events
- Handle reconnection
- Store optional local history and preferences

### 10.4 Native Interface
**Suggested first JNI API:**
```kotlin
object Flip7Native {
    init {
        System.loadLibrary("flip7core")
    }

    external fun protocolVersion(): Int

    external fun validatePacket(packet: ByteArray): Boolean

    external fun decodePacket(packet: ByteArray): ByteArray

    external fun applyIncomingPacket(
        currentState: ByteArray,
        packet: ByteArray
    ): ByteArray

    external fun createActionPacket(
        currentState: ByteArray,
        action: ByteArray
    ): ByteArray

    external fun stateForDisplay(
        currentState: ByteArray
    ): String
}
```

This API is illustrative. The exact interface should be refined while implementing the shared state container.

---

## 11. Android Operating Modes

### 11.1 Spectator Mode
Android receives state but cannot modify the game.
**Possible features:**
- Current game display
- Scores
- Puzzle progress
- Mastermind guesses
- Round history
- Connection health
- Match timer
- Statistics

This should be the first Android operating mode because it has the lowest synchronization risk.

### 11.2 Player Mode
Android behaves as a participating client.
**Possible actions:**
- Start a game
- Join a game
- Submit a move
- Enter a code
- Submit a guess
- Advance a round
- Exit a game

Player mode requires authorization and stricter conflict handling.

### 11.3 Controller Mode
Android controls the two-board system without representing a player.
**Possible actions:**
- Select game
- Reset match
- Configure players
- Manage settings
- Export history

Controller mode may be introduced after spectator mode.

---

## 12. Protocol Requirements

Every packet should contain a stable envelope.

**Suggested logical packet envelope:**
```cpp
struct PacketHeader {
    uint16_t magic;
    uint8_t protocolVersion;
    uint8_t packetType;
    uint16_t payloadLength;
    uint32_t messageId;
    uint32_t senderId;
    uint32_t epoch;
    uint32_t revision;
};
```

The actual wire representation must be serialized explicitly and must not depend on struct layout.

### 12.1 Required Fields
- Protocol magic
- Protocol version
- Packet type
- Payload length
- Message identifier
- Sender identifier
- Game identifier where applicable
- Epoch
- Revision
- Payload
- Integrity check

### 12.2 Protocol Versioning
Receivers must reject unsupported major versions. Minor versions may be accepted when backward compatible.
**Example:**
- Major version change: Breaking packet structure or behavior
- Minor version change: New optional packet or capability

### 12.3 Capability Exchange
During connection, Android and the CYD should exchange capabilities.
**Examples:**
- Supported protocol version
- Supported games
- Spectator support
- Player support
- Maximum packet size
- Fragmentation support
- Compression support
- Firmware version
- Native core version

---

## 13. Synchronization Model

The CYD pair remains the authoritative game system in the first release. Android receives synchronized snapshots and submits actions. Android should not overwrite the entire game state unless explicitly participating in recovery.

### 13.1 Action-Based Writes
**Preferred:**
```
Android sends: "Submit guess: red, blue, green, red"
```

**Not preferred:**
```
Android sends: "Replace the entire Mastermind state with this object"
```

The authoritative core validates the action and produces the resulting state.

### 13.2 Snapshots
CYDs send snapshots:
- On Android connection
- After accepted actions
- After reconciliation
- After active game changes
- After peer recovery
- On explicit snapshot request
- Periodically when appropriate

### 13.3 Conflict Resolution
Conflict resolution uses:
- Game epoch
- State revision
- Packet validity
- Active game identity
- Deterministic reconciliation rules
- Sender role where necessary

All platforms must use the shared C++ implementation for these decisions.

---

## 14. Security

### 14.1 BLE Pairing
The initial implementation should require BLE bonding or an application-level pairing code before player or controller actions are accepted. Spectator access may optionally be less restrictive.

### 14.2 Authorization Levels
**Suggested roles:**
- Spectator
- Player
- Controller
- Administrator

### 14.3 Sensitive Information
Mastermind secrets must not be exposed to an Android spectator before the round ends. The protocol should provide a sanitized view for unauthorized clients. The shared core should generate role-specific projections rather than relying only on the UI to hide sensitive fields.

### 14.4 Replay Protection
The system should reject:
- Duplicate message identifiers
- Old sequence numbers
- Stale revisions
- Packets from previous epochs
- Invalid sender identifiers
- Packets with bad integrity checks

### 14.5 Existing ESP-NOW Keys
Existing ESP-NOW PMK and LMK values remain outside the repository. BLE credentials, bonds, or application keys must also not be committed.

---

## 15. Reliability

### 15.1 Reconnection
When Android reconnects:
- Discover the Flip7 BLE service
- Read device information
- Subscribe to event notifications
- Request the active state snapshot
- Validate the snapshot through the shared core
- Resume spectator or player mode

### 15.2 Brownout Considerations
Bluetooth and display activity can increase peak current demand. The firmware should:
- Avoid unnecessary simultaneous radio operations
- Monitor resets and brownout causes
- Use a reliable USB power supply
- Avoid powering high-load peripherals from marginal USB ports
- Log Bluetooth initialization failures
- Recover cleanly after controller initialization errors

### 15.3 Watchdogs and Timeouts
The BLE layer should define:
- Connection timeout
- Fragment reassembly timeout
- Action acknowledgment timeout
- Snapshot request timeout
- Reconnection backoff
- Maximum retry count

---

## 16. Repository Structure

Recommended repository organization:
```
flip7-cyd-scorekeeper/
├── core/
│   ├── include/
│   ├── src/
│   └── tests/
│
├── firmware/
│   ├── src/
│   ├── include/
│   └── transports/
│       ├── espnow_transport.cpp
│       └── ble_transport.cpp
│
├── android/
│   ├── app/
│   │   ├── src/main/java/
│   │   ├── src/main/cpp/
│   │   │   ├── CMakeLists.txt
│   │   │   └── flip7_jni.cpp
│   │   └── src/test/
│   └── build.gradle.kts
│
├── protocol/
│   ├── specification.md
│   ├── packets.md
│   └── golden/
│
├── test/
├── platformio.ini
└── README.md
```

The repository may retain its existing structure during migration. This represents the desired end state.

---

## 17. Testing Strategy

### 17.1 Shared Core Unit Tests
Test:
- Packet encoding
- Packet decoding
- State validation
- Mastermind scoring
- Puzzle move validation
- Puzzle scrambling
- Epoch transitions
- Revision handling
- Reconciliation
- Duplicate detection
- Role-specific projections

### 17.2 Golden Packet Tests
Store known protocol examples:
- Input state
- Expected exact packet bytes
- Expected decoded state
- Expected digest

The same golden data should be tested on:
- Desktop C++
- ESP32
- Android JNI

### 17.3 BLE Tests
Test:
- Discovery
- Connection
- Service discovery
- Notification subscription
- Write acknowledgment
- MTU variation
- Fragmentation
- Reassembly
- Disconnect during transfer
- Duplicate fragment
- Missing fragment
- Reconnection
- Multiple nearby CYDs

### 17.4 Integration Tests
**Required scenarios:**
- Two CYDs continue playing with no Android device
- Android connects as spectator
- Android disconnects without interrupting the CYD game
- Android reconnects and receives current state
- Android sends a valid action
- Android sends an invalid action
- The BLE gateway CYD restarts
- The non-gateway CYD restarts
- One CYD recovers state from the other
- Android receives the reconciled state
- Android attempts to replay an old packet
- Android receives a Mastermind view without the hidden secret

### 17.5 Fuzz and Malformed Input Tests
Test:
- Truncated packets
- Invalid lengths
- Unsupported versions
- Unknown packet types
- Invalid enums
- Oversized messages
- Invalid digests
- Invalid fragment counts
- Impossible state combinations
- Unexpected reserved bytes

---

## 18. Delivery Phases

### Phase 1 — Core Extraction
- Identify platform-neutral C++ logic
- Move it into a shared core
- Remove Arduino and ESP32 dependencies
- Preserve current firmware behavior
- Add desktop unit tests
- **Exit criteria**: Existing CYD firmware builds and behaves identically using the extracted core

### Phase 2 — Protocol Hardening
- Define explicit packet serialization
- Add packet versioning
- Add packet integrity checks
- Add message identifiers
- Add origin tracking
- Create golden packet tests
- **Exit criteria**: Packet encoding is deterministic across desktop and ESP32 builds

### Phase 3 — BLE Spectator Transport
- Add the Flip7 BLE service
- Add command and event characteristics
- Implement fragmentation and reassembly
- Send snapshots to Android
- Keep Android read-only
- **Exit criteria**: Android can connect, reconnect, and display live synchronized game state

### Phase 4 — Android Native Core
- Configure Android NDK and CMake
- Compile the shared core into the APK
- Add the JNI adapter
- Run golden protocol tests on Android
- Validate incoming packets natively
- **Exit criteria**: Android and ESP32 produce matching protocol results

### Phase 5 — Android Player Actions
- Add action packet types
- Add authentication and role assignment
- Validate Android actions
- Forward accepted changes through ESP-NOW
- Add conflict and retry handling
- **Exit criteria**: Android can participate without causing state divergence

### Phase 6 — Persistence and History
- Save match history on Android
- Add export
- Add optional CYD persistence
- Add statistics and dashboards
- **Exit criteria**: Completed games survive Android restarts and can be reviewed

---

## 19. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| BLE and ESP-NOW compete for radio time | High | Keep packets small, queue transmissions, test under load |
| Native struct layouts differ | High | Use explicit serialization |
| Android and ESP32 behavior diverges | High | Compile the same C++ core on both |
| JNI becomes overly complex | Medium | Keep a narrow byte-oriented API |
| Packet forwarding causes loops | High | Add origin and message identifiers |
| Android sees hidden game information | High | Generate role-specific projections in the core |
| BLE disconnects during a game | Medium | CYDs remain authoritative and continue offline |
| Brownouts occur during radio use | Medium | Improve power delivery and log reset causes |
| Firmware size increases | Medium | Track binary size and disable unused Bluetooth Classic |
| BLE packet size is insufficient | Medium | Implement transport fragmentation |
| Multiple phones connect simultaneously | Medium | Limit initial release to one Android connection |
| Old clients send incompatible packets | Medium | Enforce protocol versions and capabilities |

---

## 20. Architectural Decisions

**ADR-001: Retain ESP-NOW Between CYDs**  
Decision: ESP-NOW remains the primary CYD-to-CYD transport.  
Reason: It is already implemented, tested, encrypted, and supports offline board synchronization.

**ADR-002: Use BLE for Android Communication**  
Decision: Android communicates with a CYD using BLE.  
Reason: Both boards have verified BLE support, and Android provides native BLE APIs.

**ADR-003: Share the C++ Core Through JNI**  
Decision: The Android APK compiles and invokes the shared C++ core using NDK and JNI.  
Reason: This avoids duplicated game and protocol implementations.

**ADR-004: Do Not Serialize Native Struct Memory**  
Decision: All protocol fields use explicit serialization.  
Reason: Native struct layouts may differ across ESP32 and Android architectures.

**ADR-005: Start With Spectator Mode**  
Decision: The first Android release is read-only.  
Reason: This validates BLE transport and JNI reuse before introducing multi-writer synchronization.

**ADR-006: Keep CYDs Authoritative**  
Decision: The CYD pair remains authoritative during the initial Android releases.  
Reason: The boards must continue operating when the phone disconnects.

**ADR-007: Use BLE Rather Than Bluetooth Classic**  
Decision: BLE is the default Android transport.  
Reason: BLE provides a structured service model, broad Android support, low-power operation, and clean notification-based state updates.

---

## 21. Definition of Done

The architecture is considered successfully implemented when:
- Both CYDs continue synchronizing over ESP-NOW
- Android discovers and connects to either CYD over BLE
- Android receives the current active game state
- Android reconnects after leaving BLE range
- Android and ESP32 compile the same shared C++ core
- Golden packet tests pass on Android and desktop
- Packets are serialized explicitly
- Android spectator mode does not expose protected game information
- BLE failure does not interrupt CYD-to-CYD play
- Duplicate and stale messages are rejected
- The firmware remains stable under simultaneous BLE and ESP-NOW activity
- Documentation explains the protocol, BLE service, and JNI interface

---

## 22. Recommended Initial Work Items

1. Extract Mastermind, puzzle, active-game, and reconciliation logic into `core/`
2. Define a versioned packet envelope
3. Replace raw struct transmission with explicit serialization
4. Add golden packet fixtures
5. Add a BLE device-information service
6. Add command and event packet characteristics
7. Implement a read-only Android BLE scanner and connection screen
8. Build the shared core through Android CMake
9. Add a minimal JNI packet validator
10. Display the synchronized active game in spectator mode

---

## 23. Final Recommendation

Implement the Android companion using:
- ESP-NOW for CYD-to-CYD synchronization
- BLE for Android-to-CYD communication
- A shared C++ core for protocol and game logic
- JNI and the Android NDK for native code reuse
- Kotlin and Jetpack Compose for Android-specific behavior
- Explicit, versioned packet serialization
- A spectator-first delivery strategy

This architecture introduces Android support without weakening the existing offline CYD system and creates a foundation for future games, transports, dashboards, and client platforms.