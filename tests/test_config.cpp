#include "riskgate/config.hpp"

#include <gtest/gtest.h>

#include <sstream>

using namespace riskgate;

TEST(Config, PerSymbolSectionsInheritDefaultsRegardlessOfOrder) {
    // [symbol AAPL] appears before [symbol_defaults]: it must still inherit.
    std::istringstream in(R"(
[symbol AAPL]
max_order_qty = 500

[symbol_defaults]
max_order_qty = 1000
max_position = 9000
price_band_bps = 250

[engine]
allow_market_orders = true
kill_switch_reject_threshold = 5
kill_switch_window_ms = 200
)");
    const auto cfg = EngineConfig::from_ini(in);
    ASSERT_EQ(cfg.symbols.count("AAPL"), 1u);
    const auto& aapl = cfg.symbols.at("AAPL");
    EXPECT_EQ(aapl.max_order_qty, 500);
    EXPECT_EQ(aapl.max_position, 9000);
    EXPECT_EQ(aapl.price_band_bps, 250u);
    EXPECT_TRUE(cfg.allow_market_orders);
    EXPECT_EQ(cfg.kill_switch_reject_threshold, 5u);
    EXPECT_EQ(cfg.kill_switch_window_ns, 200'000'000ULL);
}

TEST(Config, ParsesDecimalsExactly) {
    std::istringstream in(R"(
[symbol_defaults]
max_order_notional = 1_000_000.25   # inline comment
[symbol X]
reference_price = 0.0001
)");
    const auto cfg = EngineConfig::from_ini(in);
    EXPECT_EQ(cfg.symbol_defaults.max_order_notional, 10'000'002'500);
    EXPECT_EQ(cfg.symbols.at("X").reference_price, 1);
}

TEST(Config, RejectsUnknownKeysWithLineNumber) {
    std::istringstream in("[engine]\nmax_ordrs = 10\n");
    try {
        (void)EngineConfig::from_ini(in);
        FAIL() << "expected exception";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("line 2"), std::string::npos) << e.what();
    }
}

TEST(Config, LoadsExampleFile) {
    const auto cfg = EngineConfig::from_file(std::string(RISKGATE_SOURCE_DIR) + "/config/example.ini");
    EXPECT_TRUE(cfg.symbols.at("GME").restricted);
    EXPECT_EQ(cfg.symbols.at("AAPL").reference_price, 1'500'000);
    EXPECT_EQ(cfg.accounts.at("DESK1").orders_per_second, 500u);
    EXPECT_EQ(cfg.accounts.at("DESK1").max_open_orders, 500u) << "inherits account_defaults";
}
