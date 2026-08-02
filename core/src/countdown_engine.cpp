/**
 * Open-Source Countdown Engine - Implementation
 */

#include "flip7/countdown_engine.hpp"
#include <cctype>

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
    std::vector<int32_t> smallPool = {1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10};

    uint8_t largeToPick = std::min<uint8_t>(largeCount, 4);
    uint8_t smallToPick = 6 - largeToPick;

    std::vector<int32_t> selectedTiles;

    for (uint8_t i = 0; i < largeToPick && !largePool.empty(); ++i) {
        size_t idx = random.nextRange(0, static_cast<uint32_t>(largePool.size() - 1));
        selectedTiles.push_back(largePool[idx]);
        largePool.erase(largePool.begin() + idx);
    }

    for (uint8_t i = 0; i < smallToPick && !smallPool.empty(); ++i) {
        size_t idx = random.nextRange(0, static_cast<uint32_t>(smallPool.size() - 1));
        selectedTiles.push_back(smallPool[idx]);
        smallPool.erase(smallPool.begin() + idx);
    }

    return NumbersRound(target, selectedTiles);
}

int32_t NumbersRound::calculateScore(uint32_t target, uint32_t achieved) {
    uint32_t diff = target > achieved ? (target - achieved) : (achieved - target);
    if (diff == 0) return 10;
    if (diff <= 5) return 7;
    if (diff <= 10) return 5;
    return 0;
}

CommandResult NumbersRound::submitSolution(uint32_t boardId, uint32_t resultValue) {
    submissions_[boardId] = resultValue;
    return CommandResult{true, "", ""};
}

// --------------------------------------------------
// LettersRound
// --------------------------------------------------

bool LettersRound::drawVowel(IRandomSource& random) {
    if (drawnLetters_.size() >= 9) return false;
    const char vowels[] = {'A', 'E', 'I', 'O', 'U'};
    char v = vowels[random.nextRange(0, 4)];
    drawnLetters_ += v;
    return true;
}

bool LettersRound::drawConsonant(IRandomSource& random) {
    if (drawnLetters_.size() >= 9) return false;
    const char consonants[] = {
        'B', 'C', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'M',
        'N', 'P', 'Q', 'R', 'S', 'T', 'V', 'W', 'X', 'Y', 'Z'
    };
    char c = consonants[random.nextRange(0, 20)];
    drawnLetters_ += c;
    return true;
}

bool LettersRound::isValidSubset(const std::string& word, const std::string& availableLetters) {
    std::map<char, int> availableCounts;
    for (char c : availableLetters) {
        availableCounts[std::toupper(static_cast<unsigned char>(c))]++;
    }

    for (char c : word) {
        char upper = std::toupper(static_cast<unsigned char>(c));
        if (availableCounts[upper] <= 0) {
            return false;
        }
        availableCounts[upper]--;
    }
    return true;
}

CommandResult LettersRound::submitWord(uint32_t boardId, const std::string& word) {
    if (!isValidSubset(word, drawnLetters_)) {
        return CommandResult{false, "INVALID_WORD", "Word contains letters not in drawn set"};
    }
    submittedWords_[boardId] = word;
    return CommandResult{true, "", ""};
}

RoundResult LettersRound::evaluate(const std::vector<uint32_t>& playerBoardIds) const {
    RoundResult res;
    res.roundType = RoundType::Letters;

    size_t maxLen = 0;
    std::map<uint32_t, size_t> validLengths;

    for (uint32_t id : playerBoardIds) {
        auto it = submittedWords_.find(id);
        if (it != submittedWords_.end() && isValidSubset(it->second, drawnLetters_)) {
            size_t len = it->second.size();
            validLengths[id] = len;
            if (len > maxLen) maxLen = len;
        }
    }

    if (maxLen == 0) {
        return res;
    }

    int32_t points = (maxLen == 9) ? 18 : static_cast<int32_t>(maxLen);

    for (auto& pair : validLengths) {
        if (pair.second == maxLen) {
            res.scoreAwards[pair.first] = points;
        }
    }

    if (res.scoreAwards.size() == 1) {
        res.winnerBoardId = res.scoreAwards.begin()->first;
    } else if (res.scoreAwards.size() > 1) {
        res.isTie = true;
    }

    return res;
}

// --------------------------------------------------
// ConundrumRound
// --------------------------------------------------

ConundrumRound::ConundrumRound(const std::string& solution, const std::string& scramble)
    : solution_(solution), scramble_(scramble) {}

CommandResult ConundrumRound::buzz(uint32_t boardId) {
    if (solved_) {
        return CommandResult{false, "ALREADY_SOLVED", "Conundrum is already solved"};
    }
    if (activeBuzzerBoardId_ != 0) {
        return CommandResult{false, "BUZZER_ACTIVE", "Another player has active buzz window"};
    }
    if (std::find(failedBuzzers_.begin(), failedBuzzers_.end(), boardId) != failedBuzzers_.end()) {
        return CommandResult{false, "ALREADY_FAILED", "Player already submitted an incorrect answer"};
    }

    activeBuzzerBoardId_ = boardId;
    return CommandResult{true, "", ""};
}

CommandResult ConundrumRound::submitSolution(uint32_t boardId, const std::string& attempt) {
    if (activeBuzzerBoardId_ != boardId) {
        return CommandResult{false, "NOT_BUZZED", "Player must buzz before submitting solution"};
    }

    std::string cleanAttempt = attempt;
    std::string cleanSolution = solution_;
    for (auto& c : cleanAttempt) c = std::toupper(static_cast<unsigned char>(c));
    for (auto& c : cleanSolution) c = std::toupper(static_cast<unsigned char>(c));

    if (cleanAttempt == cleanSolution) {
        solved_ = true;
        winnerBoardId_ = boardId;
        activeBuzzerBoardId_ = 0;
        return CommandResult{true, "", ""};
    } else {
        failedBuzzers_.push_back(boardId);
        activeBuzzerBoardId_ = 0;
        return CommandResult{false, "INCORRECT_SOLUTION", "Submitted solution is incorrect"};
    }
}

// --------------------------------------------------
// CountdownMatchEngine
// --------------------------------------------------

CountdownMatchEngine::CountdownMatchEngine(uint32_t hostBoardId, uint32_t guestBoardId, uint32_t gameId)
    : gameId_(gameId), hostBoardId_(hostBoardId), guestBoardId_(guestBoardId) {
    hostPlayer_.boardId = hostBoardId;
    hostPlayer_.isHost = true;
    guestPlayer_.boardId = guestBoardId;
    guestPlayer_.isHost = false;
    phase_ = MatchPhase::Setup;
}

void CountdownMatchEngine::startNextRound(RoundType type, IRandomSource& random) {
    currentRoundType_ = type;
    roundNumber_++;
    phase_ = MatchPhase::InRound;
}

void CountdownMatchEngine::recordRoundResult(const RoundResult& result) {
    for (const auto& pair : result.scoreAwards) {
        if (pair.first == hostPlayer_.boardId) {
            hostPlayer_.score += pair.second;
        } else if (pair.first == guestPlayer_.boardId) {
            guestPlayer_.score += pair.second;
        }
    }

    phase_ = MatchPhase::BetweenRounds;

    // ADR-005 Leader-based host migration:
    // "The score leader becomes host between rounds. Ties retain the current host."
    if (guestPlayer_.score > hostPlayer_.score) {
        // Transfer host authority to guest
        std::swap(hostBoardId_, guestBoardId_);
        hostPlayer_.isHost = false;
        guestPlayer_.isHost = true;
        std::swap(hostPlayer_, guestPlayer_);
    }
}

} // namespace countdown
} // namespace flip7
