#include "riskgate/containers.hpp"
#include "riskgate/token_bucket.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>

using namespace riskgate;

TEST(FixedString, RejectsOverLength) {
    EXPECT_TRUE(FixedString<4>::from("ABCD").has_value());
    EXPECT_FALSE(FixedString<4>::from("ABCDE").has_value());
    EXPECT_EQ(FixedString<8>::from("AAPL")->view(), "AAPL");
}

TEST(FlatStringMap, InsertFindErase) {
    FlatStringMap<16, int> m(8);
    ASSERT_NE(m.insert("AAPL", 1), nullptr);
    ASSERT_NE(m.insert("MSFT", 2), nullptr);
    EXPECT_EQ(m.insert("AAPL", 3), nullptr) << "duplicate insert must fail";
    ASSERT_NE(m.find("MSFT"), nullptr);
    EXPECT_EQ(*m.find("MSFT"), 2);
    EXPECT_TRUE(m.erase("AAPL"));
    EXPECT_EQ(m.find("AAPL"), nullptr);
    EXPECT_FALSE(m.erase("AAPL"));
    ASSERT_NE(m.insert("AAPL", 4), nullptr) << "reinsert after erase";
    EXPECT_EQ(*m.find("AAPL"), 4);
    EXPECT_EQ(m.size(), 2u);
}

TEST(FlatStringMap, FailsClosedAtCapacity) {
    FlatStringMap<16, int> m(4);
    for (int i = 0; i < 4; ++i) ASSERT_NE(m.insert("K" + std::to_string(i), i), nullptr);
    EXPECT_TRUE(m.full());
    EXPECT_EQ(m.insert("K9", 9), nullptr);
}

TEST(FlatStringMap, SurvivesChurn) {
    FlatStringMap<16, int> m(64);
    for (int round = 0; round < 1000; ++round) {
        const std::string k = "ORD" + std::to_string(round);
        ASSERT_NE(m.insert(k, round), nullptr) << round;
        if (round >= 32) {
            ASSERT_TRUE(m.erase("ORD" + std::to_string(round - 32)));
        }
    }
    EXPECT_EQ(m.size(), 32u);
    for (int i = 1000 - 32; i < 1000; ++i) EXPECT_NE(m.find("ORD" + std::to_string(i)), nullptr);
}

// Backward-shift deletion: hammer a small table with colliding chains and
// check against a reference model after every operation.
TEST(FlatStringMap, BackwardShiftMatchesReferenceModel) {
    FlatStringMap<16, int> m(48);  // 128 slots
    std::map<std::string, int> ref;
    std::uint64_t rng = 99;
    auto next = [&] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    for (int step = 0; step < 200'000; ++step) {
        const std::string k = "K" + std::to_string(next() % 100);
        if (next() % 2) {
            const bool fits = ref.size() < 48;
            const bool expect = fits && !ref.count(k);
            ASSERT_EQ(m.insert(k, step) != nullptr, expect) << step;
            if (expect) ref[k] = step;
        } else {
            ASSERT_EQ(m.erase(k), ref.erase(k) == 1) << step;
        }
        if (step % 1000 == 0) {
            ASSERT_EQ(m.size(), ref.size());
            for (const auto& [rk, rv] : ref) {
                const int* v = m.find(rk);
                ASSERT_NE(v, nullptr) << rk;
                ASSERT_EQ(*v, rv);
            }
        }
    }
}

TEST(FlatU64Index, InsertFind) {
    FlatU64Index idx(16);
    EXPECT_TRUE(idx.insert(42, 7));
    EXPECT_FALSE(idx.insert(42, 8));
    EXPECT_EQ(idx.find(42), 7u);
    EXPECT_EQ(idx.find(43), 0xFFFF'FFFFu);
    EXPECT_FALSE(idx.insert(0, 1)) << "key 0 is reserved";
}

TEST(SlotPool, AcquireReleaseExhaust) {
    SlotPool<int> p(2);
    const auto a = p.acquire();
    const auto b = p.acquire();
    EXPECT_NE(a, b);
    EXPECT_EQ(p.acquire(), 0xFFFF'FFFFu);
    p.release(a);
    EXPECT_EQ(p.acquire(), a);
}

TEST(TokenBucket, BurstThenRefill) {
    constexpr Nanos t0 = 1'000'000'000;
    TokenBucket b(10, 3, t0);  // 10/s, burst 3
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(b.available(t0));
        b.consume();
    }
    EXPECT_FALSE(b.available(t0));
    EXPECT_FALSE(b.available(t0 + 99'000'000));  // 99 ms: 0.99 token
    EXPECT_TRUE(b.available(t0 + 100'000'000));  // 100 ms: 1 token
    b.consume();
    EXPECT_FALSE(b.available(t0 + 100'000'000));
    EXPECT_EQ(b.tokens(t0 + 60'000'000'000ULL), 3u) << "long idle caps at burst";
}

TEST(TokenBucket, ZeroRateIsUnlimited) {
    TokenBucket b(0, 1, 0);
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(b.available(0));
        b.consume();
    }
}
