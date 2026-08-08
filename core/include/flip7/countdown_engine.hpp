/**
 * Open-Source Countdown Engine - Core Header
 * Platform-neutral C++ implementation of Countdown game rules (Numbers, Letters, Conundrum)
 * and match-level score management.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <algorithm>

namespace flip7::countdown {

enum class RoundType : uint8_t {
    Numbers   = 1,
    Letters   = 2,
    Conundrum = 3,
};

enum class ArithmeticOp : uint8_t {
    Add      = 1,
    Subtract = 2,
    Multiply = 3,
    Divide   = 4,
};

enum class CommandType : uint8_t {
    // Numbers round
    NumSelectLargeCount   = 1,  // chooser picks 0-4 large numbers
    NumSubmitClaim        = 2,  // player submits private achieved value
    NumPresentStepRequest = 3,  // presenter requests one arithmetic step
    NumPresentBankResult  = 4,  // presenter banks current value as available
    NumPresentUndo        = 5,  // presenter undoes last step
    NumPresentComplete    = 6,  // presenter ends presentation
    // Letters round
    LetPickVowel          = 7,  // chooser draws a vowel
    LetPickConsonant      = 8,  // chooser draws a consonant
    LetSubmitClaim        = 9,  // player submits claimed word length (1-9)
    LetPresentTileTap     = 10, // presenter taps a tile by index (0-8)
    LetPresentBackspace   = 11, // presenter removes last tile
    LetPresentComplete    = 12, // presenter submits final word
    LetVerifyWord         = 13, // observer votes: payload[0] 1=accepted 0=rejected
    // Conundrum round
    ConSubmitAttempt      = 14, // player submits full 9-letter answer
};

enum class EventType : uint8_t {
    RoundStarted            = 1,
    NumbersPuzzleReady      = 2,
    NumClaimReceived        = 3,
    NumClaimsRevealed       = 4,
    NumStepAccepted         = 5,
    NumStepRejected         = 6,
    NumPresentationComplete = 7,
    LetterDrawn             = 8,
    LetterSelectionComplete = 9,
    LetClaimReceived        = 10,
    LetClaimsRevealed       = 11,
    LetWordAccepted         = 12,
    LetWordRejected         = 13,
    LetPresentationComplete = 14,
    ConAttemptCorrect       = 15,
    ConAttemptIncorrect     = 16,
    ConTimeout              = 17,
    RoundEnded              = 18,
};

enum class MatchPhase : uint8_t {
    Setup         = 0,
    InRound       = 1,
    BetweenRounds = 2,
    MatchComplete = 3,
};

// Fixed-capacity vector — no heap growth after construction; safe on ESP32.
template <typename T, size_t N>
class FixedVector {
public:
    FixedVector() : data_{}, size_(0) {}
    bool push_back(const T& v) {
        if (size_ >= N) return false;
        data_[size_++] = v;
        return true;
    }
    bool pop_back() {
        if (size_ == 0) return false;
        --size_;
        return true;
    }
    T& back() { return data_[size_ - 1]; }
    [[nodiscard]] const T& back() const { return data_[size_ - 1]; }
    T& operator[](size_t i) { return data_[i]; }
    const T& operator[](size_t i) const { return data_[i]; }
    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] bool full() const { return size_ >= N; }
    void clear() { size_ = 0; }
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    [[nodiscard]] const T* begin() const { return data_; }
    [[nodiscard]] const T* end() const { return data_ + size_; }
private:
    T    data_[N];
    size_t size_;
};

struct CalculationStep {
    int32_t     operandA{0};
    ArithmeticOp op{ArithmeticOp::Add};
    int32_t     operandB{0};
    int32_t     result{0};
};

struct NumberWorkspace {
    std::optional<int32_t>        currentValue;
    FixedVector<int32_t, 12>      availableValues;
    FixedVector<CalculationStep, 8> history;

    void reset(const std::vector<int32_t>& tiles) {
        currentValue.reset();
        availableValues.clear();
        for (auto t : tiles) availableValues.push_back(t);
        history.clear();
    }
};

struct NumberPlayerState {
    bool         claimSubmitted{false};
    int32_t      claimedValue{0};
    bool         presentationComplete{false};
    bool         presentationValid{false};
    int32_t      demonstratedValue{0};
    FixedVector<CalculationStep, 8> steps;
    NumberWorkspace workspace;
};

struct PlayerState {
    uint32_t boardId{0};
    int32_t  score{0};
    bool     isHost{false};
};

struct RoundResult {
    RoundType roundType{RoundType::Numbers};
    uint32_t  winnerBoardId{0};
    bool      isTie{false};
    std::map<uint32_t, int32_t> scoreAwards;
};

struct CommandContext {
    uint32_t senderBoardId{0};
    uint32_t timestampMs{0};
};

struct CommandResult {
    bool        accepted{false};
    std::string errorCode;
    std::string errorMessage;

    CommandResult() = default;
    CommandResult(bool acceptedValue, std::string errorCodeValue,
                  std::string errorMessageValue)
        : accepted(acceptedValue), errorCode(std::move(errorCodeValue)),
          errorMessage(std::move(errorMessageValue)) {}
};

struct EventEnvelope {
    uint32_t  sequenceNumber{0};
    uint32_t  roundId{0};
    EventType eventType;
    uint32_t  senderBoardId{0};
    std::string payload;
};

// Versioned puzzle descriptor — sent by host; guest verifies checksum before use.
struct PuzzleDescriptor {
    uint32_t roundSeed{0};
    uint16_t rulesetVersion{0};
    uint16_t contentPackVersion{0};
    uint32_t puzzleChecksum{0};
};

// Display projections — plain POD for safe copying across tasks.
struct NumbersRoundProjection {
    uint32_t target{0};
    int32_t  tiles[6]{0};
    uint8_t  tileCount{0};
};

struct LettersRoundProjection {
    char    letters[9]{0};
    uint8_t letterCount{0};
};

struct ConundrumRoundProjection {
    char     solution[10]{0};
    char     scramble[10]{0};
    char     hint[48]{0};
    bool     solved{false};
    bool     expired{false};
    uint32_t winnerBoardId{0};
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

// Returns a deterministic SeededRandomSource for a given round.
// Both boards call this with the same inputs to generate identical puzzle content.
// seed = gameId ^ (roundNumber * 2654435761u) — Knuth multiplicative hash
inline SeededRandomSource makeRoundRandom(uint32_t gameId, uint32_t roundNumber) {
    return SeededRandomSource(gameId ^ (roundNumber * 2654435761u));
}

// Round plugin interface — platform-neutral; engine holds one active round.
class IRound {
public:
    virtual ~IRound() = default;
    [[nodiscard]] virtual RoundType roundType() const = 0;
    virtual CommandResult applyCommand(const CommandContext& ctx,
                                       CommandType type,
                                       const std::vector<uint8_t>& payload) = 0;
    virtual std::optional<RoundResult> tryFinalize(
        const std::vector<uint32_t>& playerBoardIds) = 0;
};

// Numbers Round State & Logic
class NumbersRound : public IRound {
public:
    NumbersRound(uint32_t target, const std::vector<int32_t>& tiles);

    static NumbersRound createRandom(IRandomSource& random, uint8_t largeCount);

    [[nodiscard]] uint32_t target() const { return target_; }
    [[nodiscard]] const std::vector<int32_t>& tiles() const { return tiles_; }

    // Score table: exact=10, within 5=7, within 10=5, else 0.
    static int32_t calculateScore(uint32_t target, uint32_t achieved);

    // Returns true if claimedValue can be reached from tiles_ using
    // +, -, *, / operations (each tile used at most once).
    [[nodiscard]] static bool isValueReachable(int32_t target,
                                                const std::vector<int32_t>& tiles);

    // Private claim — stored by boardId; not revealed until both submitted.
    // Validates that claimedValue is reachable from tiles_; rejects unreachable
    // values with CLAIM_UNREACHABLE.
    CommandResult submitClaim(uint32_t boardId, int32_t claimedValue);
    [[nodiscard]] bool bothClaimsSubmitted() const;
    [[nodiscard]] int32_t claimFor(uint32_t boardId) const;

    // Presentation step — host validates; ContinueWithResult leaves result as
    // currentValue; BankResult returns it to the available pool for branching.
    CommandResult applyStep(uint32_t boardId, ArithmeticOp op,
                            int32_t operandA, int32_t operandB,
                            bool bankResult = false);
    CommandResult undoStep(uint32_t boardId);
    CommandResult completePresentation(uint32_t boardId);

    [[nodiscard]] const NumberPlayerState* playerState(uint32_t boardId) const;
    [[nodiscard]] NumbersRoundProjection projection() const;

    // IRound
    [[nodiscard]] RoundType roundType() const override { return RoundType::Numbers; }
    CommandResult applyCommand(const CommandContext& ctx, CommandType type,
                               const std::vector<uint8_t>& payload) override;
    std::optional<RoundResult> tryFinalize(
        const std::vector<uint32_t>& playerBoardIds) override;

private:
    uint32_t target_{0};
    std::vector<int32_t> tiles_;
    std::map<uint32_t, NumberPlayerState> playerStates_;
};

// Letters Round State & Logic
class LettersRound : public IRound {
public:
    LettersRound() = default;

    // Picking phase — called by host; enforces 3-5 vowel / 4-6 consonant constraints.
    bool drawVowel(IRandomSource& random);
    bool drawConsonant(IRandomSource& random);
    [[nodiscard]] bool canDrawVowel() const;
    [[nodiscard]] bool canDrawConsonant() const;

    [[nodiscard]] const std::string& drawnLetters() const { return drawnLetters_; }
    [[nodiscard]] size_t letterCount() const { return drawnLetters_.size(); }
    [[nodiscard]] size_t vowelCount() const { return vowelCount_; }
    [[nodiscard]] size_t consonantCount() const { return consonantCount_; }
    [[nodiscard]] bool isSelectionComplete() const { return drawnLetters_.size() >= 9; }

    bool syncDrawnLetters(const std::string& letters);

    static bool isValidSubset(const std::string& word,
                              const std::string& availableLetters);

    // Claim phase — private word-length claim per player.
    CommandResult submitClaim(uint32_t boardId, uint8_t claimedLength);
    [[nodiscard]] bool bothClaimsSubmitted() const;
    [[nodiscard]] uint8_t claimFor(uint32_t boardId) const;

    // Presentation phase — presenter taps tiles by letter index (0-8).
    CommandResult presentTileTap(uint32_t boardId, uint8_t tileIndex);
    CommandResult presentBackspace(uint32_t boardId);
    CommandResult presentComplete(uint32_t boardId);
    CommandResult verifyWord(uint32_t verifierBoardId, bool accepted);

    [[nodiscard]] const std::string& presentedWordFor(uint32_t boardId) const;
    [[nodiscard]] bool verificationFor(uint32_t boardId) const;
    [[nodiscard]] LettersRoundProjection projection() const;

    // IRound
    [[nodiscard]] RoundType roundType() const override { return RoundType::Letters; }
    CommandResult applyCommand(const CommandContext& ctx, CommandType type,
                               const std::vector<uint8_t>& payload) override;
    std::optional<RoundResult> tryFinalize(
        const std::vector<uint32_t>& playerBoardIds) override;

    [[nodiscard]] RoundResult evaluate(const std::vector<uint32_t>& playerBoardIds) const;

private:
    std::string drawnLetters_;
    size_t vowelCount_{0};
    size_t consonantCount_{0};
    std::map<uint32_t, uint8_t>     claims_;
    std::map<uint32_t, std::string> presentedWords_;
    std::map<uint32_t, bool>        verifications_;
    bool complete_{false};
};

// Conundrum Round State & Logic
// Simultaneous attempt with bounded buzz-window; both players may attempt
// until the deadline expires or the conundrum is solved.
class ConundrumRound : public IRound {
public:
    ConundrumRound(std::string  solution, std::string  scramble,
                   std::string  hint);

    // Pick a random conundrum from the built-in content table.
    static ConundrumRound createRandom(IRandomSource& random);

    [[nodiscard]] const std::string& solution() const { return solution_; }
    [[nodiscard]] const std::string& scramble() const { return scramble_; }
    [[nodiscard]] const std::string& hint()     const { return hint_; }

    // Set the submission deadline (absolute ms timestamp).
    void setDeadlineMs(uint32_t deadlineMs) { deadlineMs_ = deadlineMs; }
    [[nodiscard]] uint32_t deadlineMs() const { return deadlineMs_; }
    [[nodiscard]] bool hasDeadlineElapsed(uint32_t nowMs) const {
        return deadlineMs_ > 0 && nowMs >= deadlineMs_;
    }

    // Simultaneous attempt — checked immediately; no turn-based locking.
    // Returns accepted=true only when attempt matches solution.
    // Returns DEADLINE_ELAPSED if the buzz-window has expired.
    CommandResult submitAttempt(uint32_t boardId, const std::string& attempt);

    [[nodiscard]] bool isSolved() const { return solved_; }
    [[nodiscard]] bool isExpired() const { return expired_; }
    void markExpired();
    [[nodiscard]] uint32_t winnerBoardId() const { return winnerBoardId_; }
    [[nodiscard]] ConundrumRoundProjection projection() const;

    // IRound
    [[nodiscard]] RoundType roundType() const override { return RoundType::Conundrum; }
    CommandResult applyCommand(const CommandContext& ctx, CommandType type,
                               const std::vector<uint8_t>& payload) override;
    std::optional<RoundResult> tryFinalize(
        const std::vector<uint32_t>& playerBoardIds) override;

private:
    std::string solution_;
    std::string scramble_;
    std::string hint_;
    bool     solved_{false};
    bool     expired_{false};
    uint32_t deadlineMs_{0};
    uint32_t winnerBoardId_{0};
};

// Overall Match Manager
class CountdownMatchEngine {
public:
    CountdownMatchEngine(uint32_t hostBoardId, uint32_t guestBoardId,
                         uint32_t gameId);

    [[nodiscard]] uint32_t gameId()       const { return gameId_; }
    [[nodiscard]] uint32_t hostBoardId()  const { return hostBoardId_; }
    [[nodiscard]] uint32_t guestBoardId() const { return guestBoardId_; }
    // Score leader — owns authoritative game state.
    [[nodiscard]] uint32_t hostPlayerId()   const { return hostBoardId_; }
    // Previous round winner — picks the next round type.
    [[nodiscard]] uint32_t chooserBoardId() const { return chooserBoardId_; }
    [[nodiscard]] uint32_t hostTerm()       const { return hostTerm_; }
    [[nodiscard]] MatchPhase phase()        const { return phase_; }
    [[nodiscard]] uint32_t roundNumber()    const { return roundNumber_; }

    [[nodiscard]] const PlayerState& hostPlayer()  const { return hostPlayer_; }
    [[nodiscard]] const PlayerState& guestPlayer() const { return guestPlayer_; }

    [[nodiscard]] bool isHost(uint32_t boardId)    const { return boardId == hostBoardId_; }
    [[nodiscard]] bool isChooser(uint32_t boardId) const { return boardId == chooserBoardId_; }

    // Convenience snapshot for UI rendering.
    struct MatchState {
        PlayerState hostPlayer;
        PlayerState guestPlayer;
        MatchPhase  phase{MatchPhase::Setup};
        uint32_t    roundNumber{0};
        uint32_t    chooserBoardId{0};
        uint32_t    hostTerm{1};
    };
    [[nodiscard]] MatchState matchState() const {
        return MatchState{hostPlayer_, guestPlayer_, phase_,
                          roundNumber_, chooserBoardId_, hostTerm_};
    }

    // Reset to a fresh Setup state for a new host/guest pairing.
    void resetMatch(uint32_t hostBoardId, uint32_t guestBoardId) {
        hostBoardId_    = hostBoardId;
        guestBoardId_   = guestBoardId;
        chooserBoardId_ = hostBoardId; // host chooses the first round
        hostTerm_       = 1;
        hostPlayer_     = PlayerState{};
        hostPlayer_.boardId = hostBoardId;
        hostPlayer_.isHost  = true;
        guestPlayer_    = PlayerState{};
        guestPlayer_.boardId = guestBoardId;
        phase_          = MatchPhase::Setup;
        roundNumber_    = 0;
        activeRound_.reset();
    }

    // Reconcile authority and match metadata received from the wire without
    // discarding an already-synchronized active round.
    void syncWireState(uint32_t hostBoardId, uint32_t guestBoardId,
                       uint32_t chooserBoardId, uint32_t hostTerm,
                       uint32_t roundNumber, int32_t hostScore,
                       int32_t guestScore) {
        hostBoardId_ = hostBoardId;
        guestBoardId_ = guestBoardId;
        chooserBoardId_ = chooserBoardId;
        hostTerm_ = hostTerm;
        roundNumber_ = roundNumber;
        hostPlayer_.boardId = hostBoardId;
        hostPlayer_.isHost = true;
        hostPlayer_.score = hostScore;
        guestPlayer_.boardId = guestBoardId;
        guestPlayer_.isHost = false;
        guestPlayer_.score = guestScore;
    }

    // Create and store the active round object; advance to InRound.
    void startNextRound(RoundType type, IRandomSource& random,
                        uint8_t largeCount = 2);

    // Active round access — nullptr when no round is in progress.
    IRound*       activeRound()       { return activeRound_.get(); }
    [[nodiscard]] const IRound* activeRound() const { return activeRound_.get(); }
    [[nodiscard]] RoundType     activeRoundType()   const { return activeRoundType_; }

    // Round-type-specific accessors — nullptr when a different round type is active.
    // Uses static_cast guarded by activeRoundType_ (no RTTI / dynamic_cast needed).
    NumbersRound*        numbersRound();
    [[nodiscard]] const NumbersRound*  numbersRound()  const;
    LettersRound*        lettersRound();
    [[nodiscard]] const LettersRound*  lettersRound()  const;
    ConundrumRound*      conundrumRound();
    [[nodiscard]] const ConundrumRound* conundrumRound() const;

    // Display projections — safe to copy across tasks; return empty structs when
    // no round of that type is active.
    [[nodiscard]] NumbersRoundProjection   numbersProjection()   const;
    [[nodiscard]] LettersRoundProjection   lettersProjection()   const;
    [[nodiscard]] ConundrumRoundProjection conundrumProjection() const;

    // Apply score updates; update hostBoardId (score leader) and chooserBoardId
    // (round winner) independently per ADR-005.
    void recordRoundResult(const RoundResult& result);

    [[nodiscard]] bool isMatchComplete() const { return phase_ == MatchPhase::MatchComplete; }

private:
    uint32_t   gameId_{0};
    uint32_t   hostBoardId_{0};
    uint32_t   guestBoardId_{0};
    uint32_t   chooserBoardId_{0};
    uint32_t   hostTerm_{1};
    PlayerState hostPlayer_;
    PlayerState guestPlayer_;
    MatchPhase phase_{MatchPhase::Setup};
    uint32_t   roundNumber_{0};
    RoundType  activeRoundType_{RoundType::Numbers};
    std::unique_ptr<IRound> activeRound_;
};

} // namespace flip7::countdown
