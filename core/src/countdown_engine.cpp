/**
 * Open-Source Countdown Engine - Implementation
 */

#include "flip7/countdown_engine.hpp"
#include <cctype>
#include <cstring>
#include <numeric>

namespace flip7 {
namespace countdown {

// --------------------------------------------------
// NumbersRound
// --------------------------------------------------

NumbersRound::NumbersRound(uint32_t target, const std::vector<int32_t>& tiles)
    : target_(target), tiles_(tiles) {}

NumbersRound NumbersRound::createRandom(IRandomSource& random, uint8_t largeCount) {
    uint32_t target = random.nextRange(101, 999);

    std::vector<int32_t> largePool = {25, 50, 75, 100};
    std::vector<int32_t> smallPool = {1, 1, 2, 2, 3, 3, 4, 4, 5, 5,
                                       6, 6, 7, 7, 8, 8, 9, 9, 10, 10};

    uint8_t largeToPick = std::min<uint8_t>(largeCount, 4);
    uint8_t smallToPick = static_cast<uint8_t>(6 - largeToPick);

    std::vector<int32_t> selectedTiles;

    for (uint8_t i = 0; i < largeToPick && !largePool.empty(); ++i) {
        size_t idx = random.nextRange(0, static_cast<uint32_t>(largePool.size() - 1));
        selectedTiles.push_back(largePool[idx]);
        largePool.erase(largePool.begin() + static_cast<ptrdiff_t>(idx));
    }

    for (uint8_t i = 0; i < smallToPick && !smallPool.empty(); ++i) {
        size_t idx = random.nextRange(0, static_cast<uint32_t>(smallPool.size() - 1));
        selectedTiles.push_back(smallPool[idx]);
        smallPool.erase(smallPool.begin() + static_cast<ptrdiff_t>(idx));
    }

    return NumbersRound(target, selectedTiles);
}

int32_t NumbersRound::calculateScore(uint32_t target, uint32_t achieved) {
    uint32_t diff = target > achieved ? (target - achieved) : (achieved - target);
    if (diff == 0)  return 10;
    if (diff <= 5)  return 7;
    if (diff <= 10) return 5;
    return 0;
}

CommandResult NumbersRound::submitClaim(uint32_t boardId, int32_t claimedValue) {
    auto& ps = playerStates_[boardId];
    if (ps.claimSubmitted) {
        return CommandResult{false, "ALREADY_CLAIMED", "Claim already submitted"};
    }
    ps.claimedValue    = claimedValue;
    ps.claimSubmitted  = true;
    ps.workspace.reset(tiles_);
    return CommandResult{true, "", ""};
}

bool NumbersRound::bothClaimsSubmitted() const {
    if (playerStates_.size() < 2) return false;
    for (const auto& kv : playerStates_) {
        if (!kv.second.claimSubmitted) return false;
    }
    return true;
}

int32_t NumbersRound::claimFor(uint32_t boardId) const {
    auto it = playerStates_.find(boardId);
    return (it != playerStates_.end()) ? it->second.claimedValue : 0;
}

CommandResult NumbersRound::applyStep(uint32_t boardId, ArithmeticOp op,
                                      int32_t operandA, int32_t operandB,
                                      bool bankResult) {
    auto it = playerStates_.find(boardId);
    if (it == playerStates_.end() || !it->second.claimSubmitted) {
        return CommandResult{false, "NO_CLAIM", "No active claim for this player"};
    }
    NumberWorkspace& ws = it->second.workspace;

    // Validate operand A is available
    bool aFound = false;
    if (ws.currentValue.has_value() && ws.currentValue.value() == operandA) {
        aFound = true;
    } else {
        for (size_t i = 0; i < ws.availableValues.size(); ++i) {
            if (ws.availableValues[i] == operandA) { aFound = true; break; }
        }
    }
    // Validate operand B is available (distinct from A)
    bool bFound = false;
    for (size_t i = 0; i < ws.availableValues.size(); ++i) {
        if (ws.availableValues[i] == operandB) { bFound = true; break; }
    }
    if (!aFound || !bFound) {
        return CommandResult{false, "OPERAND_UNAVAILABLE",
                             "One or both operands are not available"};
    }

    // Compute result
    int32_t result = 0;
    switch (op) {
        case ArithmeticOp::Add:      result = operandA + operandB; break;
        case ArithmeticOp::Subtract: result = operandA - operandB; break;
        case ArithmeticOp::Multiply: result = operandA * operandB; break;
        case ArithmeticOp::Divide:
            if (operandB == 0 || operandA % operandB != 0) {
                return CommandResult{false, "INVALID_DIVISION",
                                     "Division must yield a positive integer"};
            }
            result = operandA / operandB;
            break;
    }
    if (result <= 0) {
        return CommandResult{false, "NON_POSITIVE", "Result must be positive"};
    }

    // Consume operand A from currentValue or availableValues
    if (ws.currentValue.has_value() && ws.currentValue.value() == operandA) {
        ws.currentValue.reset();
    } else {
        for (size_t i = 0; i < ws.availableValues.size(); ++i) {
            if (ws.availableValues[i] == operandA) {
                ws.availableValues[i] = ws.availableValues.back();
                ws.availableValues.pop_back();
                break;
            }
        }
    }
    // Consume operand B from availableValues
    for (size_t i = 0; i < ws.availableValues.size(); ++i) {
        if (ws.availableValues[i] == operandB) {
            ws.availableValues[i] = ws.availableValues.back();
            ws.availableValues.pop_back();
            break;
        }
    }

    CalculationStep step{operandA, op, operandB, result};
    it->second.steps.push_back(step);
    ws.history.push_back(step);

    if (bankResult) {
        ws.availableValues.push_back(result);
        ws.currentValue.reset();
    } else {
        ws.currentValue = result;  // ContinueWithResult: left operand for next step
    }

    return CommandResult{true, "", ""};
}

CommandResult NumbersRound::undoStep(uint32_t boardId) {
    auto it = playerStates_.find(boardId);
    if (it == playerStates_.end() || it->second.workspace.history.empty()) {
        return CommandResult{false, "NOTHING_TO_UNDO", "No steps to undo"};
    }
    NumberWorkspace& ws = it->second.workspace;
    const CalculationStep& last = ws.history.back();

    // Restore operands; remove result from wherever it ended up
    ws.currentValue.reset();
    ws.availableValues.push_back(last.operandA);
    ws.availableValues.push_back(last.operandB);

    // Remove the result value (it might be in currentValue or availableValues)
    // Since we always remove it when adding a new step, just restore the pair.
    ws.history.pop_back();
    if (!it->second.steps.empty()) it->second.steps.pop_back();

    // Restore currentValue to the previous step's result if any
    if (!ws.history.empty()) {
        ws.currentValue = ws.history.back().result;
        // Also remove that result from availableValues if it was banked there
    }

    return CommandResult{true, "", ""};
}

CommandResult NumbersRound::completePresentaton(uint32_t boardId) {
    auto it = playerStates_.find(boardId);
    if (it == playerStates_.end()) {
        return CommandResult{false, "UNKNOWN_PLAYER", "Unknown player"};
    }
    NumberPlayerState& ps = it->second;
    if (ps.presentationComplete) {
        return CommandResult{false, "ALREADY_COMPLETE", "Presentation already complete"};
    }
    // Determine demonstrated value: currentValue if set, else last step result
    int32_t demonstrated = 0;
    if (ps.workspace.currentValue.has_value()) {
        demonstrated = ps.workspace.currentValue.value();
    } else if (!ps.steps.empty()) {
        demonstrated = ps.steps.back().result;
    }
    ps.demonstratedValue    = demonstrated;
    ps.presentationComplete = true;
    ps.presentationValid    =
        (demonstrated == ps.claimedValue) ||
        (calculateScore(target_, static_cast<uint32_t>(demonstrated)) > 0 &&
         ps.steps.empty());  // no steps = claimed 0 or just guessing
    return CommandResult{true, "", ""};
}

const NumberPlayerState* NumbersRound::playerState(uint32_t boardId) const {
    auto it = playerStates_.find(boardId);
    return (it != playerStates_.end()) ? &it->second : nullptr;
}

NumbersRoundProjection NumbersRound::projection() const {
    NumbersRoundProjection p{};
    p.target    = target_;
    p.tileCount = static_cast<uint8_t>(std::min(tiles_.size(), size_t{6}));
    for (uint8_t i = 0; i < p.tileCount; ++i) p.tiles[i] = tiles_[i];
    return p;
}

CommandResult NumbersRound::applyCommand(const CommandContext& ctx,
                                          CommandType type,
                                          const std::vector<uint8_t>& payload) {
    switch (type) {
        case CommandType::NumSubmitClaim: {
            if (payload.size() < 4) return {false, "BAD_PAYLOAD", ""};
            int32_t v = static_cast<int32_t>(
                (uint32_t(payload[0])) | (uint32_t(payload[1]) << 8) |
                (uint32_t(payload[2]) << 16) | (uint32_t(payload[3]) << 24));
            return submitClaim(ctx.senderBoardId, v);
        }
        case CommandType::NumPresentStepRequest: {
            if (payload.size() < 9) return {false, "BAD_PAYLOAD", ""};
            auto op = static_cast<ArithmeticOp>(payload[0]);
            int32_t a = static_cast<int32_t>(
                (uint32_t(payload[1])) | (uint32_t(payload[2]) << 8) |
                (uint32_t(payload[3]) << 16) | (uint32_t(payload[4]) << 24));
            int32_t b = static_cast<int32_t>(
                (uint32_t(payload[5])) | (uint32_t(payload[6]) << 8) |
                (uint32_t(payload[7]) << 16) | (uint32_t(payload[8]) << 24));
            bool bank = (payload.size() > 9 && payload[9] != 0);
            return applyStep(ctx.senderBoardId, op, a, b, bank);
        }
        case CommandType::NumPresentBankResult:
            return undoStep(ctx.senderBoardId);  // BankResult reuses undo path
        case CommandType::NumPresentUndo:
            return undoStep(ctx.senderBoardId);
        case CommandType::NumPresentComplete:
            return completePresentaton(ctx.senderBoardId);
        default:
            return {false, "WRONG_ROUND", "Command not valid for Numbers round"};
    }
}

std::optional<RoundResult> NumbersRound::tryFinalize(
    const std::vector<uint32_t>& playerBoardIds) {
    // Finalize when both players have completed their presentations.
    bool allDone = !playerBoardIds.empty();
    for (uint32_t id : playerBoardIds) {
        auto it = playerStates_.find(id);
        if (it == playerStates_.end() || !it->second.presentationComplete) {
            allDone = false;
            break;
        }
    }
    if (!allDone) return std::nullopt;

    RoundResult res;
    res.roundType = RoundType::Numbers;

    int32_t bestScore = -1;
    for (uint32_t id : playerBoardIds) {
        auto it = playerStates_.find(id);
        if (it == playerStates_.end()) continue;
        const auto& ps = it->second;
        int32_t pts = (ps.presentationValid)
            ? calculateScore(target_, static_cast<uint32_t>(ps.demonstratedValue))
            : 0;
        res.scoreAwards[id] = pts;
        if (pts > bestScore) bestScore = pts;
    }

    uint32_t winCount = 0;
    for (auto& kv : res.scoreAwards) {
        if (kv.second == bestScore && bestScore > 0) {
            res.winnerBoardId = kv.first;
            ++winCount;
        }
    }
    if (winCount > 1) { res.isTie = true; res.winnerBoardId = 0; }

    return res;
}

// --------------------------------------------------
// LettersRound
// --------------------------------------------------

bool LettersRound::canDrawVowel() const {
    size_t remaining = 9 - drawnLetters_.size();
    if (remaining == 0) return false;
    if (vowelCount_ >= 5) return false;
    // Must leave enough slots for minimum consonants (4)
    size_t neededConsonants = (consonantCount_ < 4) ? (4 - consonantCount_) : 0;
    if (remaining <= neededConsonants) return false;
    return true;
}

bool LettersRound::canDrawConsonant() const {
    size_t remaining = 9 - drawnLetters_.size();
    if (remaining == 0) return false;
    if (consonantCount_ >= 6) return false;
    // Must leave enough slots for minimum vowels (3)
    size_t neededVowels = (vowelCount_ < 3) ? (3 - vowelCount_) : 0;
    if (remaining <= neededVowels) return false;
    return true;
}

bool LettersRound::drawVowel(IRandomSource& random) {
    if (!canDrawVowel()) return false;
    const char vowels[] = {'A', 'E', 'I', 'O', 'U'};
    char v = vowels[random.nextRange(0, 4)];
    drawnLetters_ += v;
    ++vowelCount_;
    return true;
}

bool LettersRound::drawConsonant(IRandomSource& random) {
    if (!canDrawConsonant()) return false;
    const char consonants[] = {
        'B', 'C', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'M',
        'N', 'P', 'Q', 'R', 'S', 'T', 'V', 'W', 'X', 'Y', 'Z'
    };
    char c = consonants[random.nextRange(0, 20)];
    drawnLetters_ += c;
    ++consonantCount_;
    return true;
}

bool LettersRound::isValidSubset(const std::string& word,
                                  const std::string& availableLetters) {
    std::map<char, int> counts;
    for (char c : availableLetters) {
        counts[static_cast<char>(std::toupper(static_cast<unsigned char>(c)))]++;
    }
    for (char c : word) {
        char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (counts[upper] <= 0) return false;
        counts[upper]--;
    }
    return true;
}

CommandResult LettersRound::submitClaim(uint32_t boardId, uint8_t claimedLength) {
    if (claimedLength == 0 || claimedLength > 9) {
        return CommandResult{false, "INVALID_CLAIM", "Claimed length must be 1-9"};
    }
    if (claims_.count(boardId)) {
        return CommandResult{false, "ALREADY_CLAIMED", "Claim already submitted"};
    }
    claims_[boardId] = claimedLength;
    return CommandResult{true, "", ""};
}

bool LettersRound::bothClaimsSubmitted() const {
    return claims_.size() >= 2;
}

uint8_t LettersRound::claimFor(uint32_t boardId) const {
    auto it = claims_.find(boardId);
    return (it != claims_.end()) ? it->second : 0;
}

CommandResult LettersRound::presentTileTap(uint32_t boardId, uint8_t tileIndex) {
    if (tileIndex >= drawnLetters_.size()) {
        return CommandResult{false, "BAD_INDEX", "Tile index out of range"};
    }
    std::string& word = presentedWords_[boardId];
    // Count usage of each letter already in the word vs available
    std::map<char, int> used;
    for (char c : word) used[c]++;
    std::map<char, int> pool;
    for (char c : drawnLetters_) pool[c]++;
    char tapped = drawnLetters_[tileIndex];
    if (used[tapped] >= pool[tapped]) {
        return CommandResult{false, "TILE_USED", "That letter is already fully used"};
    }
    word += tapped;
    return CommandResult{true, "", ""};
}

CommandResult LettersRound::presentBackspace(uint32_t boardId) {
    auto it = presentedWords_.find(boardId);
    if (it == presentedWords_.end() || it->second.empty()) {
        return CommandResult{false, "EMPTY", "No letters to remove"};
    }
    it->second.pop_back();
    return CommandResult{true, "", ""};
}

CommandResult LettersRound::presentComplete(uint32_t boardId) {
    const std::string& word = presentedWords_[boardId];
    uint8_t claimed = claimFor(boardId);
    if (word.size() != claimed) {
        return CommandResult{false, "LENGTH_MISMATCH",
                             "Presented word length does not match claim"};
    }
    if (!isValidSubset(word, drawnLetters_)) {
        return CommandResult{false, "INVALID_SUBSET",
                             "Word uses letters not in the drawn set"};
    }
    return CommandResult{true, "", ""};
}

CommandResult LettersRound::verifyWord(uint32_t verifierBoardId, bool accepted) {
    verifications_[verifierBoardId] = accepted;
    return CommandResult{true, "", ""};
}

const std::string& LettersRound::presentedWordFor(uint32_t boardId) const {
    static const std::string empty;
    auto it = presentedWords_.find(boardId);
    return (it != presentedWords_.end()) ? it->second : empty;
}

LettersRoundProjection LettersRound::projection() const {
    LettersRoundProjection p{};
    p.letterCount = static_cast<uint8_t>(
        std::min(drawnLetters_.size(), size_t{9}));
    for (uint8_t i = 0; i < p.letterCount; ++i) p.letters[i] = drawnLetters_[i];
    return p;
}

CommandResult LettersRound::applyCommand(const CommandContext& ctx,
                                          CommandType type,
                                          const std::vector<uint8_t>& payload) {
    switch (type) {
        case CommandType::LetPickVowel: {
            // Host-only; random source not available here — pick is pre-resolved.
            if (payload.empty()) return {false, "BAD_PAYLOAD", ""};
            char letter = static_cast<char>(payload[0]);
            drawnLetters_ += letter;
            ++vowelCount_;
            return {true, "", ""};
        }
        case CommandType::LetPickConsonant: {
            if (payload.empty()) return {false, "BAD_PAYLOAD", ""};
            char letter = static_cast<char>(payload[0]);
            drawnLetters_ += letter;
            ++consonantCount_;
            return {true, "", ""};
        }
        case CommandType::LetSubmitClaim: {
            if (payload.empty()) return {false, "BAD_PAYLOAD", ""};
            return submitClaim(ctx.senderBoardId, payload[0]);
        }
        case CommandType::LetPresentTileTap: {
            if (payload.empty()) return {false, "BAD_PAYLOAD", ""};
            return presentTileTap(ctx.senderBoardId, payload[0]);
        }
        case CommandType::LetPresentBackspace:
            return presentBackspace(ctx.senderBoardId);
        case CommandType::LetPresentComplete:
            return presentComplete(ctx.senderBoardId);
        case CommandType::LetVerifyWord: {
            if (payload.empty()) return {false, "BAD_PAYLOAD", ""};
            return verifyWord(ctx.senderBoardId, payload[0] != 0);
        }
        default:
            return {false, "WRONG_ROUND", "Command not valid for Letters round"};
    }
}

std::optional<RoundResult> LettersRound::tryFinalize(
    const std::vector<uint32_t>& playerBoardIds) {
    // Finalize when all verifications are in.
    if (verifications_.size() < playerBoardIds.size()) return std::nullopt;
    return evaluate(playerBoardIds);
}

RoundResult LettersRound::evaluate(const std::vector<uint32_t>& playerBoardIds) const {
    RoundResult res;
    res.roundType = RoundType::Letters;

    size_t maxLen = 0;
    std::map<uint32_t, size_t> validLengths;

    for (uint32_t id : playerBoardIds) {
        auto wIt = presentedWords_.find(id);
        auto vIt = verifications_.find(id);  // verifier is the opponent
        // A word is valid when: subset is correct, length matches claim,
        // and the opponent accepted it.
        bool accepted = (vIt != verifications_.end()) ? vIt->second : false;
        if (wIt != presentedWords_.end() &&
            isValidSubset(wIt->second, drawnLetters_) &&
            accepted) {
            size_t len = wIt->second.size();
            validLengths[id] = len;
            if (len > maxLen) maxLen = len;
        }
    }

    if (maxLen == 0) return res;

    int32_t points = (maxLen == 9) ? 18 : static_cast<int32_t>(maxLen);
    for (auto& kv : validLengths) {
        if (kv.second == maxLen) res.scoreAwards[kv.first] = points;
    }

    if (res.scoreAwards.size() == 1) {
        res.winnerBoardId = res.scoreAwards.begin()->first;
    } else if (res.scoreAwards.size() > 1) {
        res.isTie = true;
    }
    return res;
}

// --------------------------------------------------
// ConundrumRound — built-in word table
// TODO: replace with content/conundrum/default_words.csv -> generated header
// --------------------------------------------------

namespace {
struct ConundrumEntry { const char* solution; const char* scramble; const char* hint; };
static const ConundrumEntry kConundrumTable[] = {
    {"BLUEPRINT", "TNPULEBRI", "A detailed plan or design"},
    {"COUNTDOWN", "DTWNOCNUO", "A backward sequence before an event"},
    {"STARTLING", "GTLNRSAIT", "Surprising or astonishing"},
    {"CARPETING", "TICNGPEAR", "Floor covering material"},
    {"GRENADIER", "AIGNRDERE", "A type of soldier"},
    {"PETROLEUM", "MOUERTPLE", "Crude oil and its derivatives"},
    {"RECTANGLE", "CTRELAGNE", "A four-sided flat shape"},
    {"IMPORTANT", "TNAMIPORT", "Of great significance"},
    {"TRAMPOLIN", "MILNPAOTR", "Bouncing apparatus"},
    {"SPECTATOR", "POSTCATER", "Someone watching an event"},
};
static constexpr size_t kConundrumTableSize =
    sizeof(kConundrumTable) / sizeof(kConundrumTable[0]);
} // namespace

ConundrumRound::ConundrumRound(const std::string& solution,
                                const std::string& scramble,
                                const std::string& hint)
    : solution_(solution), scramble_(scramble), hint_(hint) {}

ConundrumRound ConundrumRound::createRandom(IRandomSource& random) {
    size_t idx = random.nextRange(0, static_cast<uint32_t>(kConundrumTableSize - 1));
    const auto& entry = kConundrumTable[idx];
    return ConundrumRound(entry.solution, entry.scramble, entry.hint);
}

CommandResult ConundrumRound::submitAttempt(uint32_t boardId,
                                             const std::string& attempt) {
    if (solved_) {
        return CommandResult{false, "ALREADY_SOLVED", "Conundrum already solved"};
    }
    std::string clean   = attempt;
    std::string correct = solution_;
    for (auto& c : clean)   c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (auto& c : correct) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (clean == correct) {
        solved_        = true;
        winnerBoardId_ = boardId;
        return CommandResult{true, "", ""};
    }
    // Wrong answer — caller handles local reshuffle; no state mutation.
    return CommandResult{false, "INCORRECT", "Incorrect answer — try again"};
}

ConundrumRoundProjection ConundrumRound::projection() const {
    ConundrumRoundProjection p{};
    std::strncpy(p.scramble, scramble_.c_str(), sizeof(p.scramble) - 1);
    std::strncpy(p.hint,     hint_.c_str(),     sizeof(p.hint) - 1);
    p.solved        = solved_;
    p.winnerBoardId = winnerBoardId_;
    return p;
}

CommandResult ConundrumRound::applyCommand(const CommandContext& ctx,
                                            CommandType type,
                                            const std::vector<uint8_t>& payload) {
    if (type != CommandType::ConSubmitAttempt) {
        return {false, "WRONG_ROUND", "Command not valid for Conundrum round"};
    }
    std::string attempt(payload.begin(), payload.end());
    return submitAttempt(ctx.senderBoardId, attempt);
}

std::optional<RoundResult> ConundrumRound::tryFinalize(
    const std::vector<uint32_t>& /*playerBoardIds*/) {
    if (!solved_) return std::nullopt;

    RoundResult res;
    res.roundType    = RoundType::Conundrum;
    res.winnerBoardId = winnerBoardId_;
    res.scoreAwards[winnerBoardId_] = 10;
    return res;
}

// --------------------------------------------------
// CountdownMatchEngine
// --------------------------------------------------

CountdownMatchEngine::CountdownMatchEngine(uint32_t hostBoardId,
                                           uint32_t guestBoardId,
                                           uint32_t gameId)
    : gameId_(gameId),
      hostBoardId_(hostBoardId),
      guestBoardId_(guestBoardId),
      chooserBoardId_(hostBoardId),
      hostTerm_(1) {
    hostPlayer_.boardId  = hostBoardId;
    hostPlayer_.isHost   = true;
    guestPlayer_.boardId = guestBoardId;
    phase_               = MatchPhase::Setup;
}

void CountdownMatchEngine::startNextRound(RoundType type, IRandomSource& random,
                                          uint8_t largeCount) {
    activeRound_.reset();
    activeRoundType_ = type;
    switch (type) {
        case RoundType::Numbers:
            activeRound_ = std::make_unique<NumbersRound>(
                NumbersRound::createRandom(random, largeCount));
            break;
        case RoundType::Letters:
            activeRound_ = std::make_unique<LettersRound>();
            break;
        case RoundType::Conundrum:
            activeRound_ = std::make_unique<ConundrumRound>(
                ConundrumRound::createRandom(random));
            break;
    }
    ++roundNumber_;
    phase_ = MatchPhase::InRound;
}

NumbersRound* CountdownMatchEngine::numbersRound() {
    return (activeRound_ && activeRoundType_ == RoundType::Numbers)
        ? static_cast<NumbersRound*>(activeRound_.get()) : nullptr;
}
const NumbersRound* CountdownMatchEngine::numbersRound() const {
    return (activeRound_ && activeRoundType_ == RoundType::Numbers)
        ? static_cast<const NumbersRound*>(activeRound_.get()) : nullptr;
}
LettersRound* CountdownMatchEngine::lettersRound() {
    return (activeRound_ && activeRoundType_ == RoundType::Letters)
        ? static_cast<LettersRound*>(activeRound_.get()) : nullptr;
}
const LettersRound* CountdownMatchEngine::lettersRound() const {
    return (activeRound_ && activeRoundType_ == RoundType::Letters)
        ? static_cast<const LettersRound*>(activeRound_.get()) : nullptr;
}
ConundrumRound* CountdownMatchEngine::conundrumRound() {
    return (activeRound_ && activeRoundType_ == RoundType::Conundrum)
        ? static_cast<ConundrumRound*>(activeRound_.get()) : nullptr;
}
const ConundrumRound* CountdownMatchEngine::conundrumRound() const {
    return (activeRound_ && activeRoundType_ == RoundType::Conundrum)
        ? static_cast<const ConundrumRound*>(activeRound_.get()) : nullptr;
}

NumbersRoundProjection CountdownMatchEngine::numbersProjection() const {
    const NumbersRound* r = numbersRound();
    return r ? r->projection() : NumbersRoundProjection{};
}
LettersRoundProjection CountdownMatchEngine::lettersProjection() const {
    const LettersRound* r = lettersRound();
    return r ? r->projection() : LettersRoundProjection{};
}
ConundrumRoundProjection CountdownMatchEngine::conundrumProjection() const {
    const ConundrumRound* r = conundrumRound();
    return r ? r->projection() : ConundrumRoundProjection{};
}

void CountdownMatchEngine::recordRoundResult(const RoundResult& result) {
    // Apply score awards.
    for (const auto& kv : result.scoreAwards) {
        if (kv.first == hostPlayer_.boardId) {
            hostPlayer_.score  += kv.second;
        } else if (kv.first == guestPlayer_.boardId) {
            guestPlayer_.score += kv.second;
        }
    }

    phase_ = MatchPhase::BetweenRounds;
    activeRound_.reset();

    // Update chooser (round winner picks next round).
    // No winner (tie or timeout) retains the previous chooser.
    if (!result.isTie && result.winnerBoardId != 0) {
        chooserBoardId_ = result.winnerBoardId;
    }

    // ADR-005 Leader-based host migration: score leader becomes host.
    // Ties retain the current host; hostTerm increments on transfer.
    if (guestPlayer_.score > hostPlayer_.score) {
        ++hostTerm_;
        std::swap(hostBoardId_, guestBoardId_);
        hostPlayer_.isHost  = false;
        guestPlayer_.isHost = true;
        std::swap(hostPlayer_, guestPlayer_);
    }
}

} // namespace countdown
} // namespace flip7
