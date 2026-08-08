# System-Oriented Architecture: Open-Source Countdown Engine

## 1. Purpose

Create a reusable, platform-neutral engine for Countdown-style games.

The engine must support:

- Numbers rounds
- Letters rounds
- Conundrum rounds
- Scorekeeping
- Match flow
- Player claims
- Presentation and verification
- Pluggable round types
- Multiple front ends
- Local and networked multiplayer
- Deterministic testing
- Embedded hardware targets
- Desktop, mobile, and web applications

The engine must not depend on:

- a specific display
- a specific touchscreen
- ESP32 APIs
- Android APIs
- browser APIs
- a specific transport
- a specific storage backend

---

## 2. Architectural goals

1. Keep game rules deterministic and testable.
2. Separate game rules from rendering and input.
3. Separate match state from round-specific state.
4. Make every round a plug-in.
5. Support authoritative-host multiplayer.
6. Allow host migration between rounds.
7. Make snapshots serializable.
8. Make replays reproducible.
9. Support custom scoring and house rules.
10. Preserve compatibility across multiple clients.

---

## 3. Top-level architecture

```text
Countdown Engine
├── Core Match Engine
│   ├── Players
│   ├── Scores
│   ├── Round order
│   ├── Current chooser
│   ├── Host authority
│   ├── Match lifecycle
│   └── Round history
│
├── Round Plug-in API
│   ├── Numbers
│   ├── Letters
│   ├── Conundrum
│   └── Future rounds
│
├── Rules Layer
│   ├── Validation
│   ├── Scoring
│   ├── Legal moves
│   ├── Claims
│   └── Results
│
├── Session Layer
│   ├── Local multiplayer
│   ├── Peer-to-peer
│   ├── Host-authoritative multiplayer
│   ├── Host migration
│   └── Snapshot recovery
│
├── Platform Adapters
│   ├── Display/UI
│   ├── Input
│   ├── Clock
│   ├── Random source
│   ├── Storage
│   ├── Networking
│   └── Audio/animation
│
└── Tooling
    ├── Test harness
    ├── Replay runner
    ├── Content validator
    ├── Simulation runner
    └── Compatibility tests
```

---

## 4. Core design boundary

The engine receives commands and produces events.

It does not directly draw screens or read buttons.

```text
Client Input
   ↓
Command
   ↓
Countdown Engine
   ↓
Validated State Change
   ↓
Event
   ↓
UI / Network / Storage Adapters
```

Example:

```text
SelectLargeNumberCount(2)
   ↓
NumbersRound validates command
   ↓
NumberSelectionUpdated
   ↓
UI redraws selection
```

This command-event model enables:

- deterministic tests
- network replication
- replay
- undo where supported
- audit logs
- headless simulation

---

## 5. Core match engine

The match engine owns state that persists across rounds.

### Responsibilities

- players
- total scores
- current round number
- current round plug-in
- previous round winner
- next-round chooser
- current host authority
- match rules
- completed round history
- match completion

### Suggested model

```cpp
using PlayerId = uint8_t;
using RoundId = uint32_t;
using MatchId = uint32_t;

struct Player {
    PlayerId id;
    std::string name;
    int score;
};

struct HostAuthority {
    PlayerId hostPlayerId;
    uint32_t hostTerm;
    uint32_t sequenceNumber;
};

struct MatchState {
    MatchId matchId;
    std::vector<Player> players;
    RoundId currentRoundId;
    std::string currentRoundType;
    PlayerId previousWinnerId;
    PlayerId chooserId;
    HostAuthority authority;
    bool complete;
};
```

---

## 6. Round plug-in contract

Every round type implements the same public contract.

```cpp
class IRound {
public:
    virtual ~IRound() = default;

    virtual std::string typeId() const = 0;
    virtual RoundSnapshot snapshot() const = 0;

    virtual CommandResult handle(
        const RoundCommand& command,
        const CommandContext& context
    ) = 0;

    virtual bool isComplete() const = 0;
    virtual RoundResult result() const = 0;
};
```

A round plug-in must define:

- state
- commands
- events
- legal transitions
- validation rules
- scoring rules
- completion rules
- serialization format
- compatibility version

---

## 7. Shared round lifecycle

Most rounds use:

```text
SETUP
  ↓
THINKING
  ↓
CLAIM
  ↓
PRESENTATION
  ↓
VERIFICATION
  ↓
RESULT
```

Not every round must use every phase.

Conundrum may use:

```text
SETUP
  ↓
ACTIVE
  ↓
ATTEMPT
  ├── WRONG → ACTIVE
  └── CORRECT → RESULT
```

The engine should model phases as round-owned state rather than hard-coding one universal sequence.

---

## 8. Numbers plug-in

### Responsibilities

- number-pool configuration
- large/small number selection
- target generation
- simultaneous thinking phase
- private final-value claims
- claim reveal
- presentation order
- arithmetic verification
- score calculation

### Core rules

Default classic-style configuration:

- 6 source numbers
- 0–4 large numbers
- remaining values from the small-number pool
- target generated within configured range
- legal operations: addition, subtraction, multiplication, division
- integer-only division
- configurable positive-intermediate-result rule
- source and intermediate values may not be reused illegally

### Suggested commands

```text
ChooseLargeCount
DrawNumbers
StartThinking
SubmitNumberClaim
RevealClaims
SelectOperand
SelectOperator
BankIntermediate
UndoStep
EndPresentation
```

### Suggested events

```text
LargeCountChosen
NumbersDrawn
TargetGenerated
ThinkingStarted
NumberClaimSubmitted
ClaimsRevealed
CalculationStepAccepted
CalculationStepRejected
PresentationCompleted
NumbersRoundCompleted
```

---

## 9. Letters plug-in

### Responsibilities

- vowel/consonant pools
- weighted letter draws
- selection limits
- simultaneous thinking phase
- private length claims
- word presentation
- letter-usage validation
- optional dictionary validation
- score calculation

### Default constraints

- exactly 9 letters
- 3–5 vowels
- 4–6 consonants
- disable choices that exceed maxima
- disable choices that would prevent satisfying remaining minima

### Suggested commands

```text
DrawVowel
DrawConsonant
StartThinking
SubmitLengthClaim
RevealClaims
SelectPresentedLetter
SubmitWord
AcceptWord
RejectWord
```

### Dictionary boundary

Dictionary validation must be behind an interface:

```cpp
class IDictionary {
public:
    virtual ~IDictionary() = default;
    virtual bool contains(std::string_view word) const = 0;
};
```

Implementations may include:

- manual confirmation
- embedded word list
- desktop dictionary
- remote dictionary service
- custom tournament dictionary

---

## 10. Conundrum plug-in

### Responsibilities

- word-and-hint content bank
- random entry selection
- scrambled layout
- per-player local order
- manual reshuffle
- forced reshuffle after wrong attempt
- automatic validation after nine selections
- first-correct winner

### Content model

```cpp
struct ConundrumEntry {
    std::string answer; // Exactly nine letters
    std::string hint;
};
```

Suggested portable format:

```text
ANSWER|HINT
BLUEPRINT|A detailed design or plan
COUNTDOWN|A backward sequence before an event
```

### Suggested commands

```text
StartConundrum
SelectLetter
ClearSelection
Reshuffle
SubmitAttempt
```

### Suggested events

```text
ConundrumLoaded
LettersShuffled
LetterSelected
AttemptRejected
ForcedReshuffle
ConundrumSolved
```

---

## 11. Commands and events

Commands express intent.

Events describe accepted facts.

### Command envelope

```cpp
struct CommandEnvelope {
    MatchId matchId;
    RoundId roundId;
    PlayerId playerId;
    uint32_t hostTerm;
    uint32_t expectedSequence;
    std::string commandType;
    ByteBuffer payload;
};
```

### Event envelope

```cpp
struct EventEnvelope {
    MatchId matchId;
    RoundId roundId;
    uint32_t hostTerm;
    uint32_t sequenceNumber;
    std::string eventType;
    ByteBuffer payload;
};
```

### Requirements

- commands are validated before state mutation
- accepted commands emit one or more events
- rejected commands return structured errors
- events are deterministic
- events can rebuild state
- sequence numbers are monotonic within a host term

---

## 12. Multiplayer model

The default multiplayer mode is host-authoritative.

### Host responsibilities

- validate commands
- generate shared random results
- advance phases
- commit scores
- publish events
- provide snapshots
- resolve round completion

### Peer responsibilities

- submit player commands
- apply authoritative events
- render local state
- preserve private local interaction where permitted
- request recovery snapshots

---

## 13. Leader-based host migration

The score leader owns the host role.

### Rules

- evaluate host ownership only after a round result is committed
- higher score becomes host
- on a tie, keep the current host
- never migrate during an active round
- old host remains authoritative until transfer commit
- new host receives the full committed snapshot

### Transfer protocol

```text
ROUND COMMITTED
  ↓
DETERMINE LEADER
  ↓
HOST_TRANSFER_PREPARE
  ↓
SNAPSHOT_SENT
  ↓
SNAPSHOT_ACKNOWLEDGED
  ↓
HOST_TRANSFER_COMMIT
  ↓
INCREMENT HOST TERM
  ↓
NEW HOST ACTIVE
```

### Split-brain prevention

Every authoritative event includes:

- host player ID
- host term
- sequence number
- match ID
- round ID

Clients reject stale authority terms.

---

## 14. Randomness

All random behavior must use an injected random source.

```cpp
class IRandomSource {
public:
    virtual ~IRandomSource() = default;
    virtual uint32_t next(uint32_t upperExclusive) = 0;
};
```

Benefits:

- deterministic tests
- reproducible replays
- seeded simulations
- platform-neutral behavior

Random decisions include:

- number draws
- target generation
- letter draws
- Conundrum entry selection
- Conundrum shuffle

---

## 15. Time

The engine must not call platform clocks directly.

```cpp
class IClock {
public:
    virtual ~IClock() = default;
    virtual uint64_t monotonicMilliseconds() const = 0;
};
```

The round engine stores deadlines and durations.

Clients decide how to render:

- analog clock
- animation
- digital countdown
- sound
- silent visual timer

---

## 16. Storage

Persistence is provided through an adapter.

```cpp
class IMatchStore {
public:
    virtual ~IMatchStore() = default;

    virtual bool saveSnapshot(
        const MatchSnapshot& snapshot
    ) = 0;

    virtual std::optional<MatchSnapshot> loadLatest(
        MatchId matchId
    ) = 0;
};
```

Possible implementations:

- flash
- SD card
- SQLite
- browser storage
- cloud database
- plain files

---

## 17. Serialization

Recommended properties:

- versioned schema
- forward-compatible fields
- explicit enum values
- deterministic encoding
- checksum support
- compact binary option for embedded targets
- human-readable JSON option for tooling

Suggested formats:

- JSON for development and debugging
- MessagePack or CBOR for embedded/network use
- event-log text format for replay tools

---

## 18. Replay and audit

Every accepted state change should be reproducible from events.

A replay file should include:

```text
MatchStarted
PlayerAdded
RoundSelected
NumbersDrawn
ThinkingStarted
ClaimSubmitted
ClaimsRevealed
CalculationStepAccepted
RoundCompleted
ScoreUpdated
HostTransferred
```

Uses:

- bug reproduction
- tournament review
- deterministic regression tests
- demo playback
- state recovery

---

## 19. Scoring policy

Scoring must be configurable and external to core round mechanics.

```cpp
class IScoringPolicy {
public:
    virtual ~IScoringPolicy() = default;

    virtual ScoreAward score(
        const RoundResult& result,
        const MatchRules& rules
    ) const = 0;
};
```

Support:

- television-style scoring
- fixed points
- house rules
- practice mode
- no-score mode
- tournament rules

---

## 20. Front-end adapters

The engine should support multiple clients.

Examples:

```text
countdown-engine-core
├── cyd-client
├── desktop-client
├── android-client
├── web-client
├── terminal-client
└── simulation-client
```

Each client implements:

- rendering
- user input
- timer presentation
- networking
- storage as needed

The client must not duplicate game rules.

---

## 21. Repository structure

```text
countdown-engine/
├── README.md
├── LICENSE
├── CONTRIBUTING.md
├── CODE_OF_CONDUCT.md
├── CMakeLists.txt
├── include/
│   └── countdown/
│       ├── core/
│       ├── match/
│       ├── rounds/
│       ├── protocol/
│       ├── serialization/
│       └── adapters/
├── src/
│   ├── core/
│   ├── match/
│   ├── rounds/
│   │   ├── numbers/
│   │   ├── letters/
│   │   └── conundrum/
│   ├── protocol/
│   └── serialization/
├── content/
│   └── conundrum/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── protocol/
│   ├── replay/
│   └── simulation/
├── tools/
│   ├── replay-runner/
│   ├── content-validator/
│   └── simulator/
├── examples/
│   ├── terminal/
│   └── local-two-player/
└── docs/
    ├── SOA.md
    ├── ROUND_PLUGIN_API.md
    ├── PROTOCOL.md
    ├── DATA_MODELS.md
    ├── TEST_STRATEGY.md
    └── ROADMAP.md
```

---

## 22. Licensing recommendation

Use a permissive license unless stronger copyleft is desired.

Recommended default:

- Apache License 2.0

Reasons:

- permissive commercial and personal use
- explicit patent grant
- friendly to embedded, mobile, and web adopters
- suitable for a reusable engine library

Content packs may use separate licenses from the engine.

---

## 23. Compatibility policy

Use semantic versioning.

```text
MAJOR.MINOR.PATCH
```

- MAJOR: incompatible API or saved-state changes
- MINOR: backward-compatible features and new rounds
- PATCH: fixes without contract changes

Every round plug-in should expose:

- plug-in ID
- plug-in version
- minimum engine version
- snapshot schema version

---

## 24. Test strategy

### Unit tests

- legal and illegal arithmetic
- number consumption
- letter limits
- claim ordering
- Conundrum attempt checking
- score calculation
- host selection
- tie behavior

### Property tests

- no source number reused illegally
- letter counts always remain valid
- shuffle preserves all letters
- event replay reaches identical state
- stale host terms never overwrite newer state

### Integration tests

- full Numbers round
- full Letters round
- full Conundrum round
- host migration
- disconnect and snapshot recovery
- save and resume

### Simulation tests

- thousands of automated matches
- randomized disconnects
- delayed and duplicated messages
- host transfer failure
- invalid client commands

---

## 25. Initial implementation roadmap

### Milestone 1: Core contracts

- MatchState
- command envelope
- event envelope
- snapshots
- deterministic random source
- fake clock
- serialization

### Milestone 2: Match engine

- players
- scores
- round registration
- chooser
- round result handling
- host authority

### Milestone 3: Numbers plug-in

- full deterministic rules
- claims
- presentation verification
- scoring integration

### Milestone 4: Letters plug-in

- constrained draws
- claims
- presentation validation
- dictionary interface

### Milestone 5: Conundrum plug-in

- word-and-hint bank
- shuffle
- attempts
- forced reshuffle
- winner detection

### Milestone 6: Network protocol

- host-authoritative commands
- events
- snapshots
- reconnect recovery
- host migration

### Milestone 7: Reference clients

- terminal client
- local two-player client
- CYD adapter

### Milestone 8: Open-source release

- license
- contribution guide
- code of conduct
- API docs
- examples
- CI
- tagged release

---

## 26. Non-goals for version 1

- online matchmaking
- cloud accounts
- voice recognition
- AI hints
- automatic dictionary downloads
- official television branding
- copyrighted music or artwork
- platform-specific UI inside the core library

---

## 27. Definition of done for version 1

Version 1 is complete when:

- the engine runs without a graphical interface
- all three rounds are playable through commands
- state can be rebuilt from events
- matches can be saved and resumed
- host migration works between rounds
- deterministic tests cover core rules
- a terminal reference client can play a complete match
- a CYD client can consume the same engine contracts
