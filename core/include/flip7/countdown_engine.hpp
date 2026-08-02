/**
 * Open-Source Countdown Engine - Core Header
 * Platform-neutral C++ implementation of Countdown game rules, round plugins,
 * command-event reducer pattern, and score management.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <algorithm>

namespace flip7 {
namespace countdown {

enum class RoundType : uint8_t {
    Numbers = 1,
    Letters = 2,
    Conundrum = 3,
};

enum class CommandType : uint8_t {
    SelectNumbersTarget = 1,
    SelectNumbersTile = 2,
    SubmitNumbersSolution = 3,
    PickLetterCategory = 4,   // 0 = Vowel, 1 = Consonant
    SubmitLettersWord = 5,
    BuzzConundrum = 6,
    SubmitConundrumSolution = 7,
};

enum class EventType : uint8_t {
    RoundStarted = 1,
    TargetSet = 2,
    TileSelected = 3,
    NumbersSolutionSubmitted = 4,
    LetterDrawn = 5,
    WordSubmitted = 6,
    ConundrumBuzzed = 7,
    ConundrumSolved = 8,
    RoundEnded = 9,
};

enum class MatchPhase : uint8_t {
    Setup = 0,
    InRound = 1,
    BetweenRounds = 2,
    MatchComplete = 3,
};

struct PlayerState {
    uint32_t boardId{0};
    int32_t score{0};
    bool isHost{false};
    bool hasBuzzed{false};
};

struct RoundDescriptor {
    std::string typeId;
    std::string version;
    RoundType roundType;
};

struct RoundResult {
    RoundType roundType;
    uint32_t winnerBoardId{0};
    bool isTie{false};
    std::map<uint32_t, int32_t> scoreAwards;
};

struct CommandContext {
    uint32_t senderBoardId{0};
    uint32_t timestampMs{0};
};

struct CommandResult {
    bool accepted{false};
    std::string errorCode;
    std::string errorMessage;

    CommandResult() = default;
    CommandResult(bool acceptedValue, std::string errorCodeValue,
                  std::string errorMessageValue)
        : accepted(acceptedValue), errorCode(std::move(errorCodeValue)),
          errorMessage(std::move(errorMessageValue)) {}
};

struct EventEnvelope {
    uint32_t sequenceNumber{0};
    uint32_t roundId{0};
    EventType eventType;
    uint32_t senderBoardId{0};
    std::string payload;
};

// Abstract Random Source Interface for Deterministic Testing
class IRandomSource {
public:
    virtual ~IRandomSource() = default;
    virtual uint32_t next() = 0;
    virtual uint32_t nextRange(uint32_t min, uint32_t max) = 0;
};

// Simple Pseudo-Random Implementation
class SeededRandomSource : public IRandomSource {
public:
    explicit SeededRandomSource(uint32_t seed = 12345) : state_(seed) {}
    uint32_t next() override {
        state_ = state_ * 1664525u + 1013904223u;
        return state_;
    }
    uint32_t nextRange(uint32_t min, uint32_t max) override {
        if (min >= max) return min;
        return min + (next() % (max - min + 1));
    }
private:
    uint32_t state_;
};

// Numbers Round State & Logic
class NumbersRound {
public:
    NumbersRound(uint32_t target, const std::vector<int32_t>& tiles);

    static NumbersRound createRandom(IRandomSource& random, uint8_t largeCount);

    uint32_t target() const { return target_; }
    const std::vector<int32_t>& tiles() const { return tiles_; }
    
    // Evaluate a solution given as result number
    static int32_t calculateScore(uint32_t target, uint32_t achieved);

    CommandResult submitSolution(uint32_t boardId, uint32_t resultValue);
    
    bool isComplete() const { return complete_; }
    uint32_t winnerBoardId() const { return winnerBoardId_; }
    const std::map<uint32_t, int32_t>& submissions() const { return submissions_; }

private:
    uint32_t target_{0};
    std::vector<int32_t> tiles_;
    std::map<uint32_t, int32_t> submissions_;
    bool complete_{false};
    uint32_t winnerBoardId_{0};
};

// Letters Round State & Logic
class LettersRound {
public:
    LettersRound() = default;

    bool drawVowel(IRandomSource& random);
    bool drawConsonant(IRandomSource& random);

    const std::string& drawnLetters() const { return drawnLetters_; }
    size_t letterCount() const { return drawnLetters_.size(); }
    bool isSelectionComplete() const { return drawnLetters_.size() >= 9; }

    static bool isValidSubset(const std::string& word, const std::string& availableLetters);
    CommandResult submitWord(uint32_t boardId, const std::string& word);

    bool isComplete() const { return complete_; }
    const std::map<uint32_t, std::string>& submittedWords() const { return submittedWords_; }
    RoundResult evaluate(const std::vector<uint32_t>& playerBoardIds) const;

private:
    std::string drawnLetters_;
    std::map<uint32_t, std::string> submittedWords_;
    bool complete_{false};
};

// Conundrum Round State & Logic
class ConundrumRound {
public:
    ConundrumRound(const std::string& solution, const std::string& scramble);

    const std::string& solution() const { return solution_; }
    const std::string& scramble() const { return scramble_; }

    CommandResult buzz(uint32_t boardId);
    CommandResult submitSolution(uint32_t boardId, const std::string& attempt);

    uint32_t activeBuzzerBoardId() const { return activeBuzzerBoardId_; }
    bool isSolved() const { return solved_; }
    uint32_t winnerBoardId() const { return winnerBoardId_; }

private:
    std::string solution_;
    std::string scramble_;
    uint32_t activeBuzzerBoardId_{0};
    bool solved_{false};
    uint32_t winnerBoardId_{0};
    std::vector<uint32_t> failedBuzzers_;
};

// Overall Match Manager
class CountdownMatchEngine {
public:
    CountdownMatchEngine(uint32_t hostBoardId, uint32_t guestBoardId, uint32_t gameId);

    uint32_t gameId() const { return gameId_; }
    uint32_t hostBoardId() const { return hostBoardId_; }
    uint32_t guestBoardId() const { return guestBoardId_; }
    MatchPhase phase() const { return phase_; }
    uint32_t roundNumber() const { return roundNumber_; }

    const PlayerState& hostPlayer() const { return hostPlayer_; }
    const PlayerState& guestPlayer() const { return guestPlayer_; }

    // Convenience read-only snapshot for UI rendering.
    struct MatchState {
        PlayerState hostPlayer;
        PlayerState guestPlayer;
        MatchPhase phase;
        uint32_t roundNumber;
    };
    MatchState matchState() const {
        return MatchState{hostPlayer_, guestPlayer_, phase_, roundNumber_};
    }

    bool isHost(uint32_t boardId) const { return boardId == hostBoardId_; }

    // Reset the match to a fresh Setup state for a new host/guest pairing.
    void resetMatch(uint32_t hostBoardId, uint32_t guestBoardId) {
        hostBoardId_ = hostBoardId;
        guestBoardId_ = guestBoardId;
        hostPlayer_ = PlayerState{};
        hostPlayer_.boardId = hostBoardId;
        hostPlayer_.isHost = true;
        guestPlayer_ = PlayerState{};
        guestPlayer_.boardId = guestBoardId;
        guestPlayer_.isHost = false;
        phase_ = MatchPhase::Setup;
        roundNumber_ = 0;
        currentRoundType_ = RoundType::Numbers;
    }

    void startNextRound(RoundType type, IRandomSource& random);
    
    // Apply score updates and perform leader-based host migration (ADR-005)
    void recordRoundResult(const RoundResult& result);

    bool isMatchComplete() const { return phase_ == MatchPhase::MatchComplete; }

private:
    uint32_t gameId_{0};
    uint32_t hostBoardId_{0};
    uint32_t guestBoardId_{0};
    PlayerState hostPlayer_;
    PlayerState guestPlayer_;
    MatchPhase phase_{MatchPhase::Setup};
    uint32_t roundNumber_{0};
    RoundType currentRoundType_{RoundType::Numbers};
};

} // namespace countdown
} // namespace flip7
