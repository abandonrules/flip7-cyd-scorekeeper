/**
 * Countdown Engine Unit Tests
 */

#include <gtest/gtest.h>
#include "flip7/countdown_engine.hpp"

using namespace flip7::countdown;

TEST(CountdownTest, NumbersRoundGenerationAndScoring) {
    SeededRandomSource random(42);
    NumbersRound round = NumbersRound::createRandom(random, 2);

    EXPECT_GE(round.target(), 101u);
    EXPECT_LE(round.target(), 999u);
    EXPECT_EQ(round.tiles().size(), 6u);

    // Test scoring table
    EXPECT_EQ(NumbersRound::calculateScore(500, 500), 10);
    EXPECT_EQ(NumbersRound::calculateScore(500, 503), 7);
    EXPECT_EQ(NumbersRound::calculateScore(500, 508), 5);
    EXPECT_EQ(NumbersRound::calculateScore(500, 515), 0);
}

TEST(CountdownTest, LettersRoundDrawingAndValidation) {
    SeededRandomSource random(1234);
    LettersRound round;

    for (int i = 0; i < 4; ++i) round.drawVowel(random);
    for (int i = 0; i < 5; ++i) round.drawConsonant(random);

    EXPECT_EQ(round.letterCount(), 9u);
    EXPECT_TRUE(round.isSelectionComplete());

    std::string drawn = round.drawnLetters();

    // Test subset validation
    EXPECT_TRUE(LettersRound::isValidSubset("", drawn));
    EXPECT_FALSE(LettersRound::isValidSubset("ZZZZZZZZZZ", drawn));

    // Submit word
    CommandResult res = round.submitWord(0x101, drawn.substr(0, 5));
    EXPECT_TRUE(res.accepted);

    std::vector<uint32_t> players = {0x101, 0x102};
    RoundResult rr = round.evaluate(players);
    EXPECT_EQ(rr.winnerBoardId, 0x101u);
    EXPECT_EQ(rr.scoreAwards[0x101], 5);
}

TEST(CountdownTest, ConundrumRoundBuzzerAndSolving) {
    ConundrumRound round("PERFECTLY", "CYPTERELF");

    EXPECT_EQ(round.solution(), "PERFECTLY");
    EXPECT_EQ(round.scramble(), "CYPTERELF");

    // Buzz player 1
    CommandResult buzzRes = round.buzz(0x201);
    EXPECT_TRUE(buzzRes.accepted);
    EXPECT_EQ(round.activeBuzzerBoardId(), 0x201u);

    // Player 2 cannot buzz while player 1 has active window
    CommandResult buzzRes2 = round.buzz(0x202);
    EXPECT_FALSE(buzzRes2.accepted);

    // Incorrect answer
    CommandResult submitWrong = round.submitSolution(0x201, "WRONGWORD");
    EXPECT_FALSE(submitWrong.accepted);
    EXPECT_EQ(round.activeBuzzerBoardId(), 0u);

    // Player 2 can now buzz
    CommandResult buzzRes3 = round.buzz(0x202);
    EXPECT_TRUE(buzzRes3.accepted);

    // Correct answer
    CommandResult submitRight = round.submitSolution(0x202, "PERFECTLY");
    EXPECT_TRUE(submitRight.accepted);
    EXPECT_TRUE(round.isSolved());
    EXPECT_EQ(round.winnerBoardId(), 0x202u);
}

TEST(CountdownTest, MatchEngineLeaderHostMigration) {
    uint32_t hostId = 0xA1;
    uint32_t guestId = 0xB2;
    uint32_t gameId = 999;

    CountdownMatchEngine engine(hostId, guestId, gameId);
    EXPECT_EQ(engine.hostBoardId(), hostId);
    EXPECT_EQ(engine.guestBoardId(), guestId);

    SeededRandomSource random(777);
    engine.startNextRound(RoundType::Numbers, random);

    RoundResult rr;
    rr.roundType = RoundType::Numbers;
    rr.scoreAwards[guestId] = 10; // Guest takes lead!
    rr.winnerBoardId = guestId;

    engine.recordRoundResult(rr);

    // ADR-005 Leader-Based Host Migration: guest should become new host!
    EXPECT_EQ(engine.hostBoardId(), guestId);
    EXPECT_EQ(engine.guestBoardId(), hostId);
}
