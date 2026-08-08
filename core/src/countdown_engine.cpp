/**
 * Open-Source Countdown Engine - Implementation
 */

#include "flip7/countdown_engine.hpp"
#include "flip7/conundrum_table.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <limits>
#include <numeric>
#include <utility>


namespace flip7::countdown {

// --------------------------------------------------
// NumbersRound
// --------------------------------------------------

NumbersRound::NumbersRound(uint32_t target, const std::vector<int32_t>& tiles)
    : target_(target), tiles_(tiles) {}

NumbersRound NumbersRound::createRandom(IRandomSource& random, uint8_t largeCount) {
    const uint32_t target = random.nextRange(101, 999);

    std::vector<int32_t> largePool = {25, 50, 75, 100};
    std::vector<int32_t> smallPool = {1, 1, 2, 2, 3, 3, 4, 4, 5, 5,
                                       6, 6, 7, 7, 8, 8, 9, 9, 10, 10};

    const uint8_t largeToPick = std::min<uint8_t>(largeCount, 4);
    const auto smallToPick = static_cast<uint8_t>(6 - largeToPick);

    std::vector<int32_t> selectedTiles;

    for (uint8_t i = 0; i < largeToPick && !largePool.empty(); ++i) {
        const size_t idx =
            random.nextRange(0, static_cast<uint32_t>(largePool.size() - 1));
        selectedTiles.push_back(largePool[idx]);
        largePool.erase(largePool.begin() + static_cast<ptrdiff_t>(idx));
    }

    for (uint8_t i = 0; i < smallToPick && !smallPool.empty(); ++i) {
        const size_t idx =
            random.nextRange(0, static_cast<uint32_t>(smallPool.size() - 1));
        selectedTiles.push_back(smallPool[idx]);
        smallPool.erase(smallPool.begin() + static_cast<ptrdiff_t>(idx));
    }

    return {target, selectedTiles};
}

int32_t NumbersRound::calculateScore(uint32_t target, uint32_t achieved) {
    uint32_t diff = target > achieved ? (target - achieved) : (achieved - target);
    if (diff == 0)  return 10;
    if (diff <= 5)  return 7;
    if (diff <= 10) return 5;
    return 0;
}

bool NumbersRound::isValueReachable(int32_t target,
                                    const std::vector<int32_t>& tiles) {
    if (tiles.empty()) return false;
    if (tiles.size() == 1) return tiles[0] == target;

    for (size_t i = 0; i < tiles.size(); ++i) {
        for (size_t j = i + 1; j < tiles.size(); ++j) {
            int32_t a = tiles[i];
            int32_t b = tiles[j];
            std::vector<int32_t> remaining;
            for (size_t k = 0; k < tiles.size(); ++k) {
                if (k != i && k != j) remaining.push_back(tiles[k]);
            }

            // a + b
            remaining.push_back(a + b);
            if (isValueReachable(target, remaining)) return true;
            remaining.pop_back();

            // a - b (positive only)
            if (a > b) {
                remaining.push_back(a - b);
                if (isValueReachable(target, remaining)) return true;
                remaining.pop_back();
            }
            // b - a (positive only)
            if (b > a) {
                remaining.push_back(b - a);
                if (isValueReachable(target, remaining)) return true;
                remaining.pop_back();
            }

            // a * b
            remaining.push_back(a * b);
            if (isValueReachable(target, remaining)) return true;
            remaining.pop_back();

            // a / b (exact division only)
            if (b != 0 && a % b == 0) {
                remaining.push_back(a / b);
                if (isValueReachable(target, remaining)) return true;
                remaining.pop_back();
            }
            // b / a (exact division only)
            if (a != 0 && b % a == 0) {
                remaining.push_back(b / a);
                if (isValueReachable(target, remaining)) return true;
                remaining.pop_back();
            }
        }
    }
    return false;
}

CommandResult NumbersRound::submitClaim(uint32_t boardId, int32_t claimedValue) {
    auto& ps = playerStates_[boardId];
    if (ps.claimSubmitted) {
        return CommandResult{false, "ALREADY_CLAIMED", "Claim already submitted"};
    }
    if (!isValueReachable(claimedValue, tiles_)) {
        return CommandResult{false, "CLAIM_UNREACHABLE",
                             "Claimed value is not achievable from the drawn tiles"};
    }
    ps.claimedValue    = claimedValue;
    ps.claimSubmitted  = true;
    ps.workspace.reset(tiles_);
    return CommandResult{true, "", ""};
}

bool NumbersRound::bothClaimsSubmitted() const {
    return playerStates_.size() >= 2 &&
           std::all_of(playerStates_.begin(), playerStates_.end(),
                       [](const auto& entry) {
                           return entry.second.claimSubmitted;
                       });
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
        for (const int32_t availableValue : ws.availableValues) {
            if (availableValue == operandA) { aFound = true; break; }
        }
    }
    // Validate operand B is available (distinct from A)
    bool bFound = false;
    for (const int32_t availableValue : ws.availableValues) {
        if (availableValue == operandB) { bFound = true; break; }
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

CommandResult NumbersRound::completePresentation(uint32_t boardId) {
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
            auto v = static_cast<int32_t>(
                (static_cast<uint32_t>(payload[0])) |
                (static_cast<uint32_t>(payload[1]) << 8) |
                (static_cast<uint32_t>(payload[2]) << 16) |
                (static_cast<uint32_t>(payload[3]) << 24));
            return submitClaim(ctx.senderBoardId, v);
        }
        case CommandType::NumPresentStepRequest: {
            if (payload.size() < 9) return {false, "BAD_PAYLOAD", ""};
            auto op = static_cast<ArithmeticOp>(payload[0]);
            auto a = static_cast<int32_t>(
                (static_cast<uint32_t>(payload[1])) |
                (static_cast<uint32_t>(payload[2]) << 8) |
                (static_cast<uint32_t>(payload[3]) << 16) |
                (static_cast<uint32_t>(payload[4]) << 24));
            auto b = static_cast<int32_t>(
                (static_cast<uint32_t>(payload[5])) |
                (static_cast<uint32_t>(payload[6]) << 8) |
                (static_cast<uint32_t>(payload[7]) << 16) |
                (static_cast<uint32_t>(payload[8]) << 24));
            bool bank = (payload.size() > 9 && payload[9] != 0);
            return applyStep(ctx.senderBoardId, op, a, b, bank);
        }
        case CommandType::NumPresentUndo:
        case CommandType::NumPresentBankResult:
            return undoStep(ctx.senderBoardId);
        case CommandType::NumPresentComplete:
            return completePresentation(ctx.senderBoardId);
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

bool LettersRound::syncDrawnLetters(const std::string& letters) {
    if (letters.size() > 9) return false;
    size_t vowels = 0;
    for (char letter : letters) {
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
        if (upper < 'A' || upper > 'Z') return false;
        if (upper == 'A' || upper == 'E' || upper == 'I' ||
            upper == 'O' || upper == 'U') {
            ++vowels;
        }
    }
    drawnLetters_ = letters;
    vowelCount_ = vowels;
    consonantCount_ = letters.size() - vowels;
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
    constexpr char vowels[] = {'A', 'E', 'I', 'O', 'U'};
    const char v = vowels[random.nextRange(0, 4)];
    drawnLetters_ += v;
    ++vowelCount_;
    return true;
}

bool LettersRound::drawConsonant(IRandomSource& random) {
    if (!canDrawConsonant()) return false;
    constexpr char consonants[] = {
        'B', 'C', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'M',
        'N', 'P', 'Q', 'R', 'S', 'T', 'V', 'W', 'X', 'Y', 'Z'
    };
    const char c = consonants[random.nextRange(0, 20)];
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

bool LettersRound::verificationFor(uint32_t boardId) const {
    auto it = verifications_.find(boardId);
    return it != verifications_.end() && it->second;
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

const ConundrumEntry kConundrumTable[] = {
    {.solution = "ADVENTURE", .scramble = "VENTUREAD", .hint = "An exciting or unusual experience"},
    {.solution = "BEAUTIFUL", .scramble = "TIFULBEAU", .hint = "Pleasant to look at"},
    {.solution = "BEGINNING", .scramble = "NINGBEGIN", .hint = "The start of something"},
    {.solution = "BLUEBERRY", .scramble = "BERRYBLUE", .hint = "A small blue edible fruit"},
    {.solution = "BLUEPRINT", .scramble = "TNPULEBRI", .hint = "A detailed plan or design"},
    {.solution = "BREAKFAST", .scramble = "FASTBREAK", .hint = "The first meal of the day"},
    {.solution = "CARPETING", .scramble = "TICNGPEAR", .hint = "Floor covering material"},
    {.solution = "CELEBRATE", .scramble = "BRATECELE", .hint = "To honor a special occasion"},
    {.solution = "CHAMPIONS", .scramble = "PIONSCHAM", .hint = "Winners of a competition"},
    {.solution = "CHOCOLATE", .scramble = "LATECHOCO", .hint = "Sweet treat made from cacao"},
    {.solution = "COMPUTERS", .scramble = "PUTERCOMS", .hint = "Electronic information machines"},
    {.solution = "COUNTDOWN", .scramble = "DTWNOCNUO", .hint = "A backward sequence before an event"},
    {.solution = "CREATURES", .scramble = "TURESCREA", .hint = "Living beings or animals"},
    {.solution = "CROSSWORD", .scramble = "WORDCROSS", .hint = "A word puzzle with intersecting answers"},
    {.solution = "DANGEROUS", .scramble = "GEROUSDAN", .hint = "Likely to cause harm"},
    {.solution = "DISCOVERY", .scramble = "COVERYDIS", .hint = "The finding of something new"},
    {.solution = "EDUCATION", .scramble = "CATIONEDU", .hint = "The process of learning"},
    {.solution = "ELEPHANTS", .scramble = "PHANTSELE", .hint = "Large animals with trunks"},
    {.solution = "EVERGREEN", .scramble = "GREENEVER", .hint = "A plant that stays green year-round"},
    {.solution = "FESTIVALS", .scramble = "TIVALSFES", .hint = "Large celebrations or events"},
    {.solution = "FIREPLACE", .scramble = "PLACEFIRE", .hint = "A hearth for a fire"},
    {.solution = "GARDENING", .scramble = "DENINGGAR", .hint = "The activity of growing plants"},
    {.solution = "GRENADIER", .scramble = "AIGNRDERE", .hint = "A type of soldier"},
    {.solution = "HAPPINESS", .scramble = "PINESSHAP", .hint = "A state of joy"},
    {.solution = "HEADLIGHT", .scramble = "LIGHTHEAD", .hint = "A forward-facing vehicle lamp"},
    {.solution = "IMPORTANT", .scramble = "TNAMIPORT", .hint = "Of great significance"},
    {.solution = "JELLYFISH", .scramble = "LYFISHJEL", .hint = "A drifting sea animal"},
    {.solution = "KEYBOARDS", .scramble = "BOARDKEYS", .hint = "Devices with keys for entering text"},
    {.solution = "KNOWLEDGE", .scramble = "LEDGEKNOW", .hint = "Information and understanding"},
    {.solution = "LANDSCAPE", .scramble = "SCAPELAND", .hint = "The visible features of an area"},
    {.solution = "LIGHTNING", .scramble = "NINGLIGHT", .hint = "A flash of electricity in a storm"},
    {.solution = "MAGNETISM", .scramble = "NETISMMAG", .hint = "A force that attracts iron"},
    {.solution = "MOUNTAINS", .scramble = "TAINSMOUN", .hint = "Very high natural elevations"},
    {.solution = "MUSICALLY", .scramble = "CALLYMUSI", .hint = "In a way related to music"},
    {.solution = "NOTEBOOKS", .scramble = "BOOKSNOTE", .hint = "Books used for writing notes"},
    {.solution = "ORCHESTRA", .scramble = "ESTRAORCH", .hint = "A large group of instrumentalists"},
    {.solution = "PAINTINGS", .scramble = "TINGSPAIN", .hint = "Pictures made with paint"},
    {.solution = "PAPERBACK", .scramble = "BACKPAPER", .hint = "A book with a flexible cover"},
    {.solution = "PETROLEUM", .scramble = "MOUERTPLE", .hint = "Crude oil and its derivatives"},
    {.solution = "PINEAPPLE", .scramble = "APPLEPINE", .hint = "A tropical fruit with spiky skin"},
    {.solution = "POLITICAL", .scramble = "TICALPOLI", .hint = "Relating to government"},
    {.solution = "RECTANGLE", .scramble = "CTRELAGNE", .hint = "A four-sided flat shape"},
    {.solution = "SOMETHING", .scramble = "THINGSOME", .hint = "An unspecified object or matter"},
    {.solution = "SPECTATOR", .scramble = "POSTCATER", .hint = "Someone watching an event"},
    {.solution = "STARTLING", .scramble = "GTLNRSAIT", .hint = "Surprising or astonishing"},
    {.solution = "SUNFLOWER", .scramble = "FLOWERSUN", .hint = "A tall plant with a yellow bloom"},
    {.solution = "TELESCOPE", .scramble = "SCOPETELE", .hint = "An instrument for viewing distant objects"},
    {.solution = "UNDERLINE", .scramble = "LINEUNDER", .hint = "A line drawn beneath text"},
    {.solution = "WONDERFUL", .scramble = "DERFULWON", .hint = "Extremely good"},
    {.solution = "WORKPLACE", .scramble = "PLACEWORK", .hint = "A location where people work"},
};
const std::size_t kConundrumTableSize =
    std::size(kConundrumTable);
static_assert(std::size(kConundrumTable) - 1 <=
              std::numeric_limits<uint32_t>::max(),
              "Conundrum table indices must fit IRandomSource::nextRange");

ConundrumRound::ConundrumRound(std::string  solution,
                                std::string  scramble,
                                std::string  hint)
    : solution_(std::move(solution)), scramble_(std::move(scramble)), hint_(std::move(hint)) {}

ConundrumRound ConundrumRound::createRandom(IRandomSource& random) {
    if (kConundrumTableSize == 0) {
        return ConundrumRound{"", "", ""};
    }
    size_t idx = random.nextRange(0, static_cast<uint32_t>(kConundrumTableSize - 1));
    const auto& entry = kConundrumTable[idx];
    return {entry.solution, entry.scramble, entry.hint};
}

CommandResult ConundrumRound::submitAttempt(uint32_t boardId,
                                             const std::string& attempt) {
    if (solved_) {
        return CommandResult{false, "ALREADY_SOLVED", "Conundrum already solved"};
    }
    if (expired_) {
        return CommandResult{false, "DEADLINE_ELAPSED", "Buzz-window has expired"};
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
    std::strncpy(p.solution, solution_.c_str(), sizeof(p.solution) - 1);
    std::strncpy(p.scramble, scramble_.c_str(), sizeof(p.scramble) - 1);
    std::strncpy(p.hint,     hint_.c_str(),     sizeof(p.hint) - 1);
    p.solved        = solved_;
    p.expired       = expired_;
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

void ConundrumRound::markExpired() {
    expired_ = true;
}

std::optional<RoundResult> ConundrumRound::tryFinalize(
    const std::vector<uint32_t>& /*playerBoardIds*/) {
    if (!solved_ && !expired_) return std::nullopt;
    if (expired_ && !solved_) {
        RoundResult res;
        res.roundType = RoundType::Conundrum;
        res.isTie = true;
        return res;
    }

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
        ? static_cast<NumbersRound*>(activeRound_.get()) // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        : nullptr;
}
const NumbersRound* CountdownMatchEngine::numbersRound() const {
    return (activeRound_ && activeRoundType_ == RoundType::Numbers)
        ? static_cast<const NumbersRound*>(activeRound_.get()) // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        : nullptr;
}
LettersRound* CountdownMatchEngine::lettersRound() {
    return (activeRound_ && activeRoundType_ == RoundType::Letters)
        ? static_cast<LettersRound*>(activeRound_.get()) // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        : nullptr;
}
const LettersRound* CountdownMatchEngine::lettersRound() const {
    return (activeRound_ && activeRoundType_ == RoundType::Letters)
        ? static_cast<const LettersRound*>(activeRound_.get()) // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        : nullptr;
}
ConundrumRound* CountdownMatchEngine::conundrumRound() {
    return (activeRound_ && activeRoundType_ == RoundType::Conundrum)
        ? static_cast<ConundrumRound*>(activeRound_.get()) // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        : nullptr;
}
const ConundrumRound* CountdownMatchEngine::conundrumRound() const {
    return (activeRound_ && activeRoundType_ == RoundType::Conundrum)
        ? static_cast<const ConundrumRound*>(activeRound_.get()) // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        : nullptr;
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

} // namespace flip7::countdown
