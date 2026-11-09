#include "riskgate/risk_engine.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace riskgate;

namespace {

constexpr Nanos T0 = 1'000'000'000;

EngineConfig base_config() {
    EngineConfig c;
    c.max_orders = 1024;
    c.max_cl_ord_ids = 4096;
    c.max_symbols = 64;
    c.max_accounts = 16;
    c.max_positions = 256;
    c.symbol_defaults.max_order_qty = 1'000;
    c.symbol_defaults.max_order_notional = to_price(200'000.0);
    c.symbol_defaults.max_position = 2'000;
    c.symbol_defaults.price_band_bps = 500;  // 5%
    c.symbol_defaults.reference_price = to_price(100.0);
    c.account_defaults.max_open_orders = 100;
    c.account_defaults.max_open_notional = to_price(1'000'000.0);
    c.account_defaults.orders_per_second = 0;  // unlimited unless a test sets it
    return c;
}

NewOrderRequest buy(std::string_view id, Qty qty, double px, std::string_view sym = "AAPL") {
    return {id, "ACC", sym, Side::Buy, OrdType::Limit, qty, to_price(px)};
}

NewOrderRequest sell(std::string_view id, Qty qty, double px, std::string_view sym = "AAPL") {
    return {id, "ACC", sym, Side::Sell, OrdType::Limit, qty, to_price(px)};
}

RejectReason reason(CheckResult r) { return r.reason; }

} // namespace

// ── Basic acceptance and reservation ─────────────────────────────────────────

TEST(RiskEngine, AcceptsAndReservesExposure) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(buy("A1", 100, 101.0), T0));
    const auto p = e.position("ACC", "AAPL");
    EXPECT_EQ(p.open_buy_qty, 100);
    EXPECT_EQ(p.position, 0);
    EXPECT_EQ(e.open_notional("ACC"), to_price(101.0) * 100);
    EXPECT_EQ(e.open_orders("ACC"), 1u);
    EXPECT_EQ(e.live_order_count(), 1u);
}

TEST(RiskEngine, InvalidOrders) {
    RiskEngine e(base_config());
    EXPECT_EQ(reason(e.check_new(buy("A1", 0, 100.0), T0)), RejectReason::InvalidOrder);
    EXPECT_EQ(reason(e.check_new(buy("A2", 10, 0.0), T0)), RejectReason::InvalidOrder);
    EXPECT_EQ(reason(e.check_new(buy("", 10, 100.0), T0)), RejectReason::InvalidOrder);
    EXPECT_EQ(reason(e.check_new(buy(std::string(33, 'X'), 10, 100.0), T0)), RejectReason::ClOrdIdTooLong);
}

TEST(RiskEngine, DuplicateClOrdIdEvenAfterOrderIsDone) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(buy("A1", 10, 100.0), T0));
    EXPECT_EQ(reason(e.check_new(buy("A1", 10, 100.0), T0)), RejectReason::DuplicateClOrdId);
    ASSERT_TRUE(e.on_fill("A1", 10, to_price(100.0)));
    EXPECT_EQ(e.live_order_count(), 0u);
    EXPECT_EQ(reason(e.check_new(buy("A1", 10, 100.0), T0)), RejectReason::DuplicateClOrdId)
        << "ClOrdIDs must stay unique for the whole session";
}

// ── Reference data ───────────────────────────────────────────────────────────

TEST(RiskEngine, RestrictedAndUnknownSymbols) {
    auto c = base_config();
    c.symbols["GME"] = c.symbol_defaults;
    c.symbols["GME"].restricted = true;
    c.allow_unconfigured_symbols = false;
    c.symbols["AAPL"] = c.symbol_defaults;
    RiskEngine e(c);
    EXPECT_EQ(reason(e.check_new(buy("A1", 10, 100.0, "GME"), T0)), RejectReason::RestrictedSymbol);
    EXPECT_EQ(reason(e.check_new(buy("A2", 10, 100.0, "ZZZZ"), T0)), RejectReason::UnknownSymbol);
    EXPECT_TRUE(e.check_new(buy("A3", 10, 100.0, "AAPL"), T0));
}

TEST(RiskEngine, UnknownAccountWhenUnconfiguredAccountsDisabled) {
    auto c = base_config();
    c.allow_unconfigured_accounts = false;
    c.accounts["DESK1"] = c.account_defaults;
    RiskEngine e(c);
    EXPECT_EQ(reason(e.check_new(buy("A1", 10, 100.0), T0)), RejectReason::UnknownAccount);
    NewOrderRequest r = buy("A2", 10, 100.0);
    r.account = "DESK1";
    EXPECT_TRUE(e.check_new(r, T0));
}

// ── Price collar ─────────────────────────────────────────────────────────────

TEST(RiskEngine, PriceBandIsInclusiveAndSymmetric) {
    RiskEngine e(base_config());  // ref 100, band 5%
    EXPECT_TRUE(e.check_new(buy("A1", 1, 105.0), T0));
    EXPECT_EQ(reason(e.check_new(buy("A2", 1, 105.01), T0)), RejectReason::PriceOutsideBand);
    EXPECT_TRUE(e.check_new(sell("A3", 1, 95.0, "MSFT"), T0));
    EXPECT_EQ(reason(e.check_new(sell("A4", 1, 94.99, "MSFT"), T0)), RejectReason::PriceOutsideBand);
}

TEST(RiskEngine, NoReferencePrice) {
    auto c = base_config();
    c.symbol_defaults.reference_price = 0;
    RiskEngine e(c);
    EXPECT_EQ(reason(e.check_new(buy("A1", 1, 100.0), T0)), RejectReason::NoReferencePrice);
    e.set_reference_price("AAPL", to_price(100.0));
    EXPECT_TRUE(e.check_new(buy("A2", 1, 100.0), T0));
}

TEST(RiskEngine, FillsMoveTheReference) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(buy("A1", 10, 104.0), T0));
    ASSERT_TRUE(e.on_fill("A1", 10, to_price(104.0)));
    EXPECT_EQ(e.reference_price("AAPL"), to_price(104.0));
    EXPECT_TRUE(e.check_new(buy("A2", 1, 109.0), T0)) << "109 is within 5% of 104";
}

TEST(RiskEngine, MarketOrders) {
    auto c = base_config();
    RiskEngine off(c);
    NewOrderRequest m{"M1", "ACC", "AAPL", Side::Buy, OrdType::Market, 10, 0};
    EXPECT_EQ(reason(off.check_new(m, T0)), RejectReason::MarketOrdersDisabled);

    c.allow_market_orders = true;
    RiskEngine on(c);
    EXPECT_TRUE(on.check_new(m, T0));
    EXPECT_EQ(on.open_notional("ACC"), to_price(100.0) * 10) << "priced at reference";
}

// ── Fat finger ───────────────────────────────────────────────────────────────

TEST(RiskEngine, MaxOrderQtyAndNotional) {
    RiskEngine e(base_config());
    EXPECT_EQ(reason(e.check_new(buy("A1", 1'001, 100.0), T0)), RejectReason::MaxOrderQty);
    auto c = base_config();
    c.symbol_defaults.max_order_qty = 0;
    RiskEngine e2(c);
    EXPECT_TRUE(e2.check_new(buy("A2", 2'000, 100.0), T0));  // exactly 200k
    EXPECT_EQ(reason(e2.check_new(buy("A3", 2'001, 100.0), T0)), RejectReason::MaxOrderNotional);
}

TEST(RiskEngine, NotionalOverflowFailsClosed) {
    auto c = base_config();
    c.symbol_defaults = SymbolLimits{};  // no limits at all
    c.symbol_defaults.require_reference_price = false;
    RiskEngine e(c);
    NewOrderRequest r = buy("A1", INT64_MAX / 2, 100.0);
    EXPECT_FALSE(e.check_new(r, T0));
}

// ── Position limits (worst case) ─────────────────────────────────────────────

TEST(RiskEngine, OpenOrdersCountTowardPosition) {
    RiskEngine e(base_config());  // max_position 2000, max order 1000
    ASSERT_TRUE(e.check_new(buy("A1", 1'000, 100.0), T0));
    ASSERT_TRUE(e.check_new(buy("A2", 1'000, 100.0), T0));
    EXPECT_EQ(reason(e.check_new(buy("A3", 1, 100.0), T0)), RejectReason::MaxPosition);
    // Selling is still allowed: it reduces worst-case long.
    EXPECT_TRUE(e.check_new(sell("S1", 500, 104.0), T0));
}

TEST(RiskEngine, FillsMoveExposureFromOpenToPosition) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(buy("A1", 1'000, 100.0), T0));
    ASSERT_TRUE(e.on_new_ack("A1"));
    ASSERT_TRUE(e.on_fill("A1", 400, to_price(100.0)));
    auto p = e.position("ACC", "AAPL");
    EXPECT_EQ(p.position, 400);
    EXPECT_EQ(p.open_buy_qty, 600);
    EXPECT_EQ(e.open_notional("ACC"), to_price(100.0) * 600);
    ASSERT_TRUE(e.on_fill("A1", 600, to_price(100.0)));
    p = e.position("ACC", "AAPL");
    EXPECT_EQ(p.position, 1'000);
    EXPECT_EQ(p.open_buy_qty, 0);
    EXPECT_EQ(e.open_orders("ACC"), 0u);
    EXPECT_EQ(e.live_order_count(), 0u);
}

TEST(RiskEngine, ShortSideLimit) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(sell("S1", 1'000, 100.0), T0));
    ASSERT_TRUE(e.on_fill("S1", 1'000, to_price(100.0)));
    ASSERT_TRUE(e.check_new(sell("S2", 1'000, 100.0), T0));
    EXPECT_EQ(reason(e.check_new(sell("S3", 1, 100.0), T0)), RejectReason::MaxPosition);
    EXPECT_EQ(e.position("ACC", "AAPL").position, -1'000);
}

// ── Account limits ───────────────────────────────────────────────────────────

TEST(RiskEngine, MaxOpenNotionalAcrossSymbols) {
    auto c = base_config();
    c.account_defaults.max_open_notional = to_price(150'000.0);
    RiskEngine e(c);
    ASSERT_TRUE(e.check_new(buy("A1", 1'000, 100.0, "AAPL"), T0));
    EXPECT_EQ(reason(e.check_new(buy("A2", 501, 100.0, "MSFT"), T0)), RejectReason::MaxOpenNotional);
    EXPECT_TRUE(e.check_new(buy("A3", 500, 100.0, "MSFT"), T0));
}

TEST(RiskEngine, MaxOpenOrders) {
    auto c = base_config();
    c.account_defaults.max_open_orders = 2;
    RiskEngine e(c);
    ASSERT_TRUE(e.check_new(buy("A1", 1, 100.0), T0));
    ASSERT_TRUE(e.check_new(buy("A2", 1, 100.0), T0));
    EXPECT_EQ(reason(e.check_new(buy("A3", 1, 100.0), T0)), RejectReason::MaxOpenOrders);
    ASSERT_TRUE(e.on_cancelled("A1"));
    EXPECT_TRUE(e.check_new(buy("A4", 1, 100.0), T0));
}

// ── Throttle ─────────────────────────────────────────────────────────────────

TEST(RiskEngine, RateLimitOnlySpendsTokensOnAcceptedOrders) {
    auto c = base_config();
    c.account_defaults.orders_per_second = 2;
    c.account_defaults.burst = 2;
    RiskEngine e(c);
    EXPECT_EQ(reason(e.check_new(buy("X1", 5'000, 100.0), T0)), RejectReason::MaxOrderQty);
    ASSERT_TRUE(e.check_new(buy("A1", 1, 100.0), T0));
    ASSERT_TRUE(e.check_new(buy("A2", 1, 100.0), T0)) << "rejected X1 must not have used a token";
    EXPECT_EQ(reason(e.check_new(buy("A3", 1, 100.0), T0)), RejectReason::RateLimited);
    EXPECT_TRUE(e.check_new(buy("A4", 1, 100.0), T0 + 500'000'000));
}

TEST(RiskEngine, CancelsAreNeverThrottled) {
    auto c = base_config();
    c.account_defaults.orders_per_second = 1;
    c.account_defaults.burst = 1;
    RiskEngine e(c);
    ASSERT_TRUE(e.check_new(buy("A1", 1, 100.0), T0));
    EXPECT_EQ(reason(e.check_new(buy("A2", 1, 100.0), T0)), RejectReason::RateLimited);
    EXPECT_TRUE(e.check_cancel({"C1", "A1"}, T0));
}

// ── Self-trade prevention ────────────────────────────────────────────────────

TEST(RiskEngine, SelfTradePrevention) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(sell("S1", 10, 101.0), T0));
    EXPECT_TRUE(e.check_new(buy("B1", 10, 100.99), T0)) << "does not cross";
    EXPECT_EQ(reason(e.check_new(buy("B2", 10, 101.0), T0)), RejectReason::SelfTrade);
    EXPECT_EQ(reason(e.check_new(sell("S2", 10, 100.99), T0)), RejectReason::SelfTrade);

    // Other accounts are independent.
    NewOrderRequest other = buy("B3", 10, 101.0);
    other.account = "OTHER";
    EXPECT_TRUE(e.check_new(other, T0));
}

TEST(RiskEngine, SelfTradePreventionCanBeDisabled) {
    auto c = base_config();
    c.self_trade_prevention = false;
    RiskEngine e(c);
    ASSERT_TRUE(e.check_new(sell("S1", 10, 101.0), T0));
    EXPECT_TRUE(e.check_new(buy("B1", 10, 101.0), T0));
}

// ── Cancel / replace lifecycle ───────────────────────────────────────────────

TEST(RiskEngine, CancelLifecycle) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(buy("A1", 100, 100.0), T0));
    EXPECT_EQ(reason(e.check_cancel({"C1", "NOPE"}, T0)), RejectReason::UnknownOrder);
    ASSERT_TRUE(e.check_cancel({"C1", "A1"}, T0));
    EXPECT_EQ(reason(e.check_cancel({"C2", "A1"}, T0)), RejectReason::OrderNotOpen) << "cancel already pending";
    ASSERT_TRUE(e.on_cancel_rejected("C1"));
    ASSERT_TRUE(e.check_cancel({"C3", "A1"}, T0)) << "cancel allowed again after reject";
    ASSERT_TRUE(e.on_cancelled("C3")) << "venue may report the cancel under the cancel's ClOrdID";
    EXPECT_EQ(e.live_order_count(), 0u);
    EXPECT_EQ(e.position("ACC", "AAPL").open_buy_qty, 0);
}

TEST(RiskEngine, ReplaceReservesMaxOfOldAndNewUntilAck) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(buy("A1", 500, 100.0), T0));
    ASSERT_TRUE(e.on_new_ack("A1"));

    // Downsize: until the venue confirms, the old 500 may still be live.
    ASSERT_TRUE(e.check_replace({"A2", "A1", 200, to_price(100.0)}, T0));
    EXPECT_EQ(e.position("ACC", "AAPL").open_buy_qty, 500);
    ASSERT_TRUE(e.on_replaced("A2"));
    EXPECT_EQ(e.position("ACC", "AAPL").open_buy_qty, 200);

    // Upsize: reserve the new size immediately.
    ASSERT_TRUE(e.check_replace({"A3", "A2", 900, to_price(100.0)}, T0));
    EXPECT_EQ(e.position("ACC", "AAPL").open_buy_qty, 900);
    ASSERT_TRUE(e.on_replace_rejected("A3"));
    EXPECT_EQ(e.position("ACC", "AAPL").open_buy_qty, 200) << "reject restores the old reservation";

    // Old ID must no longer address the order after a successful replace.
    EXPECT_EQ(reason(e.check_cancel({"C1", "A1"}, T0)), RejectReason::UnknownOrder);
    EXPECT_TRUE(e.check_cancel({"C2", "A2"}, T0));
}

TEST(RiskEngine, ReplaceChecksLimitsAgainstNewTerms) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(buy("A1", 500, 100.0), T0));
    ASSERT_TRUE(e.on_new_ack("A1"));
    EXPECT_EQ(reason(e.check_replace({"A2", "A1", 1'001, to_price(100.0)}, T0)), RejectReason::MaxOrderQty);
    EXPECT_EQ(reason(e.check_replace({"A3", "A1", 500, to_price(110.0)}, T0)), RejectReason::PriceOutsideBand);
    EXPECT_EQ(reason(e.check_replace({"A4", "NOPE", 500, to_price(100.0)}, T0)), RejectReason::UnknownOrder);
    EXPECT_EQ(reason(e.check_replace({"A1", "A1", 400, to_price(100.0)}, T0)), RejectReason::DuplicateClOrdId);
}

TEST(RiskEngine, ReplaceBelowFilledQtyIsInvalid) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(buy("A1", 500, 100.0), T0));
    ASSERT_TRUE(e.on_fill("A1", 300, to_price(100.0)));
    EXPECT_EQ(reason(e.check_replace({"A2", "A1", 300, to_price(100.0)}, T0)), RejectReason::InvalidOrder);
    EXPECT_TRUE(e.check_replace({"A3", "A1", 400, to_price(100.0)}, T0));
    EXPECT_EQ(e.position("ACC", "AAPL").open_buy_qty, 200) << "max(old leaves 200, new leaves 100)";
}

TEST(RiskEngine, ReplaceWhilePendingNewIsRejected) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(buy("A1", 500, 100.0), T0));
    EXPECT_EQ(reason(e.check_replace({"A2", "A1", 400, to_price(100.0)}, T0)), RejectReason::OrderNotOpen);
}

TEST(RiskEngine, VenueRejectReleasesExposure) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(buy("A1", 500, 100.0), T0));
    ASSERT_TRUE(e.on_order_rejected("A1"));
    EXPECT_EQ(e.position("ACC", "AAPL").open_buy_qty, 0);
    EXPECT_EQ(e.open_notional("ACC"), 0);
    EXPECT_FALSE(e.on_fill("A1", 10, to_price(100.0))) << "unknown after close";
}

// ── Kill switch ──────────────────────────────────────────────────────────────

TEST(RiskEngine, ManualKillSwitchBlocksNewAndReplaceButNotCancel) {
    RiskEngine e(base_config());
    ASSERT_TRUE(e.check_new(buy("A1", 10, 100.0), T0));
    ASSERT_TRUE(e.on_new_ack("A1"));
    e.engage_kill_switch();
    EXPECT_EQ(reason(e.check_new(buy("A2", 10, 100.0), T0)), RejectReason::KillSwitch);
    EXPECT_EQ(reason(e.check_replace({"A3", "A1", 5, to_price(100.0)}, T0)), RejectReason::KillSwitch);
    EXPECT_TRUE(e.check_cancel({"C1", "A1"}, T0));

    std::vector<std::string> live;
    e.for_each_open_order([&](const OrderView& o) { live.emplace_back(o.cl_ord_id); });
    ASSERT_EQ(live.size(), 1u);
    EXPECT_EQ(live[0], "A1");

    e.release_kill_switch();
    EXPECT_TRUE(e.check_new(buy("A4", 10, 100.0), T0));
}

TEST(RiskEngine, AutoKillSwitchOnRejectBurst) {
    auto c = base_config();
    c.kill_switch_reject_threshold = 3;
    c.kill_switch_window_ns = 1'000'000;  // 1 ms
    RiskEngine e(c);

    // Three rejects spread over 3 ms: never three inside 1 ms.
    for (int i = 0; i < 3; ++i)
        (void)e.check_new(buy("X" + std::to_string(i), 5'000, 100.0), T0 + i * 1'500'000ULL);
    EXPECT_FALSE(e.kill_switch_engaged());

    // Three rejects inside 1 ms: trip.
    const Nanos t = T0 + 10'000'000;
    for (int i = 0; i < 3; ++i)
        (void)e.check_new(buy("Y" + std::to_string(i), 5'000, 100.0), t + i * 100'000ULL);
    EXPECT_TRUE(e.kill_switch_engaged());
    EXPECT_EQ(reason(e.check_new(buy("Z1", 1, 100.0), t)), RejectReason::KillSwitch);
}

// ── Capacity ─────────────────────────────────────────────────────────────────

TEST(RiskEngine, FailsClosedWhenOrderTableIsFull) {
    auto c = base_config();
    c.max_orders = 2;
    c.account_defaults.max_open_orders = 0;
    RiskEngine e(c);
    ASSERT_TRUE(e.check_new(buy("A1", 1, 100.0), T0));
    ASSERT_TRUE(e.check_new(buy("A2", 1, 100.0), T0));
    EXPECT_EQ(reason(e.check_new(buy("A3", 1, 100.0), T0)), RejectReason::CapacityExceeded);
    ASSERT_TRUE(e.on_cancelled("A1"));
    EXPECT_TRUE(e.check_new(buy("A4", 1, 100.0), T0));
}

TEST(RiskEngine, StatsCountRejectsByReason) {
    RiskEngine e(base_config());
    (void)e.check_new(buy("A1", 1, 100.0), T0);
    (void)e.check_new(buy("A1", 1, 100.0), T0);
    (void)e.check_new(buy("A2", 5'000, 100.0), T0);
    EXPECT_EQ(e.stats().checks, 3u);
    EXPECT_EQ(e.stats().accepts, 1u);
    EXPECT_EQ(e.stats().rejects_by_reason[static_cast<int>(RejectReason::DuplicateClOrdId)], 1u);
    EXPECT_EQ(e.stats().rejects_by_reason[static_cast<int>(RejectReason::MaxOrderQty)], 1u);
}

// ── Randomised invariant check ───────────────────────────────────────────────
// Drive a long random sequence of orders, acks, fills, cancels and replaces
// and verify that the aggregates always equal the sum over live orders.

TEST(RiskEngine, AggregatesMatchLiveOrdersUnderRandomFlow) {
    auto c = base_config();
    c.symbol_defaults.max_position = 0;
    c.account_defaults.max_open_notional = 0;
    c.account_defaults.max_open_orders = 0;
    c.self_trade_prevention = false;
    RiskEngine e(c);

    std::uint64_t rng = 12345;
    auto next = [&] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    std::vector<std::string> ids;
    int seq = 0;

    for (int step = 0; step < 20'000; ++step) {
        const auto op = next() % 6;
        if (op <= 1 || ids.empty()) {
            const std::string id = "O" + std::to_string(seq++);
            const Side side = next() % 2 ? Side::Buy : Side::Sell;
            const Qty q = 1 + static_cast<Qty>(next() % 500);
            const Price px = to_price(96.0) + static_cast<Price>(next() % 80'000);
            if (e.check_new({id, "ACC", "AAPL", side, OrdType::Limit, q, px}, T0)) ids.push_back(id);
        } else {
            const std::string& id = ids[next() % ids.size()];
            switch (op) {
                case 2: (void)e.on_new_ack(id); break;
                case 3: (void)e.on_fill(id, 1 + static_cast<Qty>(next() % 200), to_price(100.0)); break;
                case 4: (void)e.on_cancelled(id); break;
                case 5: {
                    const std::string nid = "R" + std::to_string(seq++);
                    if (e.check_replace({nid, id, 1 + static_cast<Qty>(next() % 600), to_price(100.0)}, T0)) {
                        if (next() % 2) (void)e.on_replaced(nid); else (void)e.on_replace_rejected(nid);
                        ids.push_back(nid);
                    }
                    break;
                }
            }
        }

        if (step % 997 == 0) {
            Qty buy_open = 0, sell_open = 0;
            std::uint32_t n = 0;
            e.for_each_open_order([&](const OrderView& o) {
                ++n;
                const Qty leaves = o.qty - o.cum_qty;
                (o.side == Side::Buy ? buy_open : sell_open) += leaves;
            });
            const auto p = e.position("ACC", "AAPL");
            ASSERT_EQ(p.open_buy_qty, buy_open) << "step " << step;
            ASSERT_EQ(p.open_sell_qty, sell_open) << "step " << step;
            ASSERT_EQ(e.open_orders("ACC"), n);
            ASSERT_EQ(e.live_order_count(), n);
        }
    }
}
