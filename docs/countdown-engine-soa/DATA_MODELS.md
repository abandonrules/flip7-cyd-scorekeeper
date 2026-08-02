# Core Data Models

```cpp
struct MatchRules {
    std::string scoringPolicyId;
    uint32_t thinkingDurationMs;
    bool allowHouseRules;
};

struct RoundResult {
    RoundId roundId;
    std::string roundType;
    std::optional<PlayerId> winnerId;
    std::map<PlayerId, int> scoreAwards;
    bool tie;
};

struct MatchSnapshot {
    uint32_t schemaVersion;
    MatchState match;
    ByteBuffer currentRoundSnapshot;
    uint32_t checksum;
};

struct CommandError {
    std::string code;
    std::string message;
    bool retryable;
};

struct CommandResult {
    bool accepted;
    std::vector<EventEnvelope> events;
    std::optional<CommandError> error;
};
```
