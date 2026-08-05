/**
 * Countdown Engine Unit Tests
 */

#include <gtest/gtest.h>
#include "flip7/countdown_engine.hpp"

using namespace flip7::countdown;

// ---------------------------------------------------------------------------
// makeRoundRandom — determinism
// ---------------------------------------------------------------------------

TEST(CountdownTest, MakeRoundRandomIsDeterministic) {
    auto r1 = makeRoundRandom(0xABCD1234u, 3u);
    auto r2 = makeRoundRandom(0xABCD1234u, 3u);
    EXPECT_EQ(r1.next(), r2.next());
    EXPECT_EQ(r1.next(), r2.next());
}

TEST(CountdownTest, MakeRoundRandomDiffersAcrossRounds) {
    auto r1 = makeRoundRandom(42u, 1u);
    auto r2 = makeRoundRandom(42u, 2u);
    EXPECT_NE(r1.next(), r2.next());
}

// ---------------------------------------------------------------------------
// NumbersRound — generation, scoring, claims, steps
// ---------------------------------------------------------------------------

TEST(CountdownTest, NumbersRoundGenerationAndScoring) {
    SeededRandomSource random(42);
    NumbersRound round = NumbersRound::createRandom(random, 2);

    EXPECT_GE(round.target(), 101u);
    EXPECT_LE(round.target(), 999u);
    EXPECT_EQ(round.tiles().size(), 6u);

    EXPECT_EQ(NumbersRound::calculateScore(500, 500), 10);
    EXPECT_EQ(NumbersRound::calculateScore(500, 503), 7);
    EXPECT_EQ(NumbersRound::calculateScore(500, 508), 5);
    EXPECT_EQ(NumbersRound::calculateScore(500, 515), 0);
}

TEST(CountdownTest, NumbersRoundClaims) {
    SeededRandomSource random(1);
    NumbersRound round = NumbersRound::createRandom(random, 0);

    EXPECT_FALSE(round.bothClaimsSubmitted());

    auto r1 = round.submitClaim(0x11, 480);
    EXPECT_TRUE(r1.accepted);
    EXPECT_FALSE(round.bothClaimsSubmitted());

    auto r2 = round.submitClaim(0x22, 495);
    EXPECT_TRUE(r2.accepted);
    EXPECT_TRUE(round.bothClaimsSubmitted());

    // Duplicate claim rejected
    auto r3 = round.submitClaim(0x11, 490);
    EXPECT_FALSE(r3.accepted);
    EXPECT_EQ(r3.errorCode, "ALREADY_CLAIMED");

    EXPECT_EQ(round.claimFor(0x11), 480);
    EXPECT_EQ(round.claimFor(0x22), 495);
}

TEST(CountdownTest, NumbersRoundArithmeticStep) {
    // Simple round: target 100, tiles [25, 50, 4, 2, 1, 1]
    NumbersRound round(100, {25, 50, 4, 2, 1, 1});
    round.submitClaim(0x01, 100);

    // 25 + 50 = 75
    auto r1 = round.applyStep(0x01, ArithmeticOp::Add, 25, 50);
    EXPECT_TRUE(r1.accepted);

    // 75 + 25 would fail — 25 is consumed
    auto r2 = round.applyStep(0x01, ArithmeticOp::Add, 75, 25);
    EXPECT_FALSE(r2.accepted);  // 25 no longer available

    // 75 + 4 = 79
    auto r3 = round.applyStep(0x01, ArithmeticOp::Add, 75, 4);
    EXPECT_TRUE(r3.accepted);

    // Undo
    auto r4 = round.undoStep(0x01);
    EXPECT_TRUE(r4.accepted);
}

TEST(CountdownTest, NumbersRoundDivisionValidation) {
    NumbersRound round(300, {100, 25, 4, 3, 2, 1});
    round.submitClaim(0x01, 300);

    // Valid integer division: 100 / 4 = 25
    auto r1 = round.applyStep(0x01, ArithmeticOp::Divide, 100, 4);
    EXPECT_TRUE(r1.accepted);

    // Non-integer division rejected: 25 / 4
    round.submitClaim(0x02, 0);
    auto r2 = round.applyStep(0x02, ArithmeticOp::Divide, 25, 4);
    EXPECT_FALSE(r2.accepted);
    EXPECT_EQ(r2.errorCode, "INVALID_DIVISION");
}

// ---------------------------------------------------------------------------
// LettersRound — picking constraints, claims, tile presentation
// ---------------------------------------------------------------------------

TEST(CountdownTest, LettersRoundPickingConstraints) {
    SeededRandomSource random(1234);
    LettersRound round;

    // Draw 4 vowels; 5th should be blocked (max 5 but we need room for 4 consonants)
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(round.canDrawVowel());
        EXPECT_TRUE(round.drawVowel(random));
    }

    // At 4 vowels, 0 consonants, 5 remaining → vowel still allowed (max is 5)
    EXPECT_TRUE(round.canDrawVowel());

    // Draw 3 consonants
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(round.canDrawConsonant());
        EXPECT_TRUE(round.drawConsonant(random));
    }

    // Now 4 vowels, 3 consonants, 2 remaining — must have at least 1 more consonant
    // Vowel still ok (4 < 5 and remaining 2 > neededConsonants 1)
    EXPECT_TRUE(round.canDrawVowel());

    // Draw 5th vowel
    EXPECT_TRUE(round.drawVowel(random));
    // Now 5 vowels — vowel button disabled
    EXPECT_FALSE(round.canDrawVowel());

    // Draw last consonant
    EXPECT_TRUE(round.drawConsonant(random));
    EXPECT_EQ(round.letterCount(), 9u);
    EXPECT_TRUE(round.isSelectionComplete());
}

TEST(CountdownTest, LettersRoundValidSubset) {
    SeededRandomSource random(1234);
    LettersRound round;
    for (int i = 0; i < 4; ++i) round.drawVowel(random);
    for (int i = 0; i < 5; ++i) round.drawConsonant(random);

    EXPECT_EQ(round.letterCount(), 9u);
    EXPECT_TRUE(LettersRound::isValidSubset("", round.drawnLetters()));
    EXPECT_FALSE(LettersRound::isValidSubset("ZZZZZZZZZZ", round.drawnLetters()));
}

TEST(CountdownTest, LettersRoundClaimsAndPresentation) {
    LettersRound round;
    // Manually set letters for determinism
    for (char c : std::string("AEIORCNTS")) {
        // Use applyCommand to add each letter
        CommandContext ctx{0x10, 0};
        std::vector<uint8_t> payload = {static_cast<uint8_t>(c)};
        round.applyCommand(ctx, CommandType::LetPickVowel, payload);
    }

    // Claim length 5
    auto r1 = round.submitClaim(0x101, 5);
    EXPECT_TRUE(r1.accepted);

    // Duplicate claim rejected
    EXPECT_FALSE(round.submitClaim(0x101, 6).accepted);

    // Length must be 1-9
    EXPECT_FALSE(round.submitClaim(0x102, 0).accepted);
    EXPECT_TRUE(round.submitClaim(0x102, 7).accepted);
    EXPECT_TRUE(round.bothClaimsSubmitted());
}

// ---------------------------------------------------------------------------
// ConundrumRound — simultaneous attempts (no buzz)
// ---------------------------------------------------------------------------

TEST(CountdownTest, ConundrumRoundSimultaneousAttempts) {
    ConundrumRound round("BLUEPRINT", "TNPULEBRI", "A detailed plan or design");

    EXPECT_EQ(round.solution(), "BLUEPRINT");
    EXPECT_EQ(round.hint(),     "A detailed plan or design");
    EXPECT_FALSE(round.isSolved());

    // Player 1 incorrect — no lockout
    auto r1 = round.submitAttempt(0x201, "WRONGWORD");
    EXPECT_FALSE(r1.accepted);
    EXPECT_FALSE(round.isSolved());

    // Player 2 can immediately attempt — simultaneous
    auto r2 = round.submitAttempt(0x202, "BLUEPRINT");
    EXPECT_TRUE(r2.accepted);
    EXPECT_TRUE(round.isSolved());
    EXPECT_EQ(round.winnerBoardId(), 0x202u);
}

TEST(CountdownTest, ConundrumRoundAlreadySolved) {
    ConundrumRound round("BLUEPRINT", "TNPULEBRI", "A hint");
    round.submitAttempt(0x201, "BLUEPRINT");

    // Further attempts rejected once solved
    auto r = round.submitAttempt(0x202, "BLUEPRINT");
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.errorCode, "ALREADY_SOLVED");
}

TEST(CountdownTest, ConundrumRoundCreateRandom) {
    auto random = makeRoundRandom(999u, 5u);
    ConundrumRound round = ConundrumRound::createRandom(random);

    EXPECT_FALSE(round.solution().empty());
    EXPECT_FALSE(round.scramble().empty());
    EXPECT_FALSE(round.hint().empty());
    EXPECT_EQ(round.solution().size(), 9u);
}

// ---------------------------------------------------------------------------
// MatchEngine — startNextRound creates active round; dual-role recordRoundResult
// ---------------------------------------------------------------------------

TEST(CountdownTest, MatchEngineStartNextRoundCreatesRound) {
    CountdownMatchEngine engine(0xA1, 0xB2, 999);

    EXPECT_EQ(engine.activeRound(), nullptr);

    auto random = makeRoundRandom(999u, 1u);
    engine.startNextRound(RoundType::Numbers, random, 2);

    EXPECT_NE(engine.activeRound(), nullptr);
    EXPECT_EQ(engine.activeRound()->roundType(), RoundType::Numbers);
    EXPECT_NE(engine.numbersRound(), nullptr);
    EXPECT_EQ(engine.lettersRound(), nullptr);
    EXPECT_EQ(engine.conundrumRound(), nullptr);
}

TEST(CountdownTest, MatchEngineLeaderHostMigrationAndChooserUpdate) {
    uint32_t hostId   = 0xA1;
    uint32_t guestId  = 0xB2;
    uint32_t gameId   = 999;

    CountdownMatchEngine engine(hostId, guestId, gameId);
    EXPECT_EQ(engine.hostBoardId(),   hostId);
    EXPECT_EQ(engine.guestBoardId(),  guestId);
    EXPECT_EQ(engine.chooserBoardId(), hostId);  // host chooses first
    EXPECT_EQ(engine.hostTerm(), 1u);

    auto random = makeRoundRandom(gameId, 1u);
    engine.startNextRound(RoundType::Numbers, random, 2);

    // Guest wins the round — becomes chooser AND takes score lead → becomes host
    RoundResult rr;
    rr.roundType     = RoundType::Numbers;
    rr.winnerBoardId = guestId;
    rr.scoreAwards[guestId] = 10;

    engine.recordRoundResult(rr);

    // ADR-005: guest took score lead → guest becomes new host
    EXPECT_EQ(engine.hostBoardId(),  guestId);
    EXPECT_EQ(engine.guestBoardId(), hostId);
    EXPECT_EQ(engine.hostTerm(), 2u);  // incremented on transfer

    // chooser = round winner = guestId
    EXPECT_EQ(engine.chooserBoardId(), guestId);
}

TEST(CountdownTest, MatchEngineChooserRetainedOnTie) {
    CountdownMatchEngine engine(0xA1, 0xB2, 1);
    auto random = makeRoundRandom(1u, 1u);
    engine.startNextRound(RoundType::Letters, random);

    // Tie round — no winner
    RoundResult rr;
    rr.roundType = RoundType::Letters;
    rr.isTie     = true;
    rr.scoreAwards[0xA1] = 5;
    rr.scoreAwards[0xB2] = 5;

    uint32_t prevChooser = engine.chooserBoardId();
    engine.recordRoundResult(rr);

    // Tie: previous chooser retained; no host migration (scores equal)
    EXPECT_EQ(engine.chooserBoardId(), prevChooser);
    EXPECT_EQ(engine.hostTerm(), 1u);  // no transfer
}

// ---------------------------------------------------------------------------
// IRound interface — applyCommand dispatch
// ---------------------------------------------------------------------------

TEST(CountdownTest, IRoundNumbersApplyCommandClaim) {
    auto random = makeRoundRandom(10u, 1u);
    NumbersRound round = NumbersRound::createRandom(random, 2);

    CommandContext ctx{0x01, 0};
    // Encode claimed value 350 as little-endian uint32
    std::vector<uint8_t> payload = {0x5E, 0x01, 0x00, 0x00};  // 0x15E = 350
    auto result = round.applyCommand(ctx, CommandType::NumSubmitClaim, payload);
    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(round.claimFor(0x01), 350);
}

TEST(CountdownTest, IRoundConundrumApplyCommand) {
    ConundrumRound round("STARTLING", "GTLNRSAIT", "Surprising");
    CommandContext ctx{0x10, 0};
    std::vector<uint8_t> attempt{'S','T','A','R','T','L','I','N','G'};
    auto result = round.applyCommand(ctx, CommandType::ConSubmitAttempt, attempt);
    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(round.isSolved());
}

// ---------------------------------------------------------------------------
// FixedVector
// ---------------------------------------------------------------------------

TEST(CountdownTest, FixedVectorCapacity) {
    FixedVector<int, 3> v;
    EXPECT_TRUE(v.empty());
    EXPECT_TRUE(v.push_back(1));
    EXPECT_TRUE(v.push_back(2));
    EXPECT_TRUE(v.push_back(3));
    EXPECT_TRUE(v.full());
    EXPECT_FALSE(v.push_back(4));  // over capacity
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], 1);
    EXPECT_TRUE(v.pop_back());
    EXPECT_EQ(v.size(), 2u);
    EXPECT_FALSE(v.full());
}

