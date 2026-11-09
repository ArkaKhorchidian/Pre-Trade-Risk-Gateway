#include "riskgate/fix_gateway.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace riskgate;
using namespace riskgate::fix;

namespace {

constexpr Nanos T0 = 1'000'000'000;

EngineConfig cfg() {
    EngineConfig c;
    c.max_orders = 256;
    c.max_cl_ord_ids = 1024;
    c.symbol_defaults.max_order_qty = 1'000;
    c.symbol_defaults.max_position = 5'000;
    c.symbol_defaults.price_band_bps = 500;
    c.symbol_defaults.reference_price = to_price(150.0);
    return c;
}

std::string nos(std::string_view id, std::string_view qty, std::string_view px, char side = '1') {
    return "35=D|11=" + std::string(id) + "|1=DESK1|55=AAPL|54=" + std::string(1, side) +
           "|38=" + std::string(qty) + "|40=2|44=" + std::string(px) + "|59=0|";
}

} // namespace

TEST(FixView, ParsesPipeAndSoh) {
    const FixView a("8=FIX.4.2|35=D|11=ABC|55=AAPL|");
    EXPECT_EQ(a.msg_type(), "D");
    EXPECT_EQ(a.get(11), "ABC");
    EXPECT_FALSE(a.has(44));

    const std::string soh = std::string("35=8\x01") + "150=2\x01" + "32=100\x01";
    const FixView b(soh);
    EXPECT_EQ(b.get(150), "2");
    EXPECT_EQ(b.get(32), "100");
}

TEST(FixNumbers, ExactDecimalParsing) {
    Price p = 0;
    ASSERT_TRUE(parse_price("150.25", p));
    EXPECT_EQ(p, 1'502'500);
    ASSERT_TRUE(parse_price("0.0001", p));
    EXPECT_EQ(p, 1);
    ASSERT_TRUE(parse_price("150.123456", p));
    EXPECT_EQ(p, 1'501'234) << "sub-tick digits truncate, never round up";
    EXPECT_FALSE(parse_price("", p));
    EXPECT_FALSE(parse_price("1.2.3", p));
    EXPECT_FALSE(parse_price("abc", p));

    Qty q = 0;
    ASSERT_TRUE(parse_qty("100", q));
    EXPECT_EQ(q, 100);
    ASSERT_TRUE(parse_qty("100.00", q));
    EXPECT_EQ(q, 100);
    EXPECT_FALSE(parse_qty("100.5", q));
    EXPECT_EQ(format_price(1'502'500), "150.2500");
}

TEST(FixFrame, BodyLengthAndChecksum) {
    const std::string app = std::string("35=0") + kSOH;
    const std::string msg = frame_fix("FIX.4.2", "RISK", "CLIENT", 7, "20260101-09:30:00.000", app);
    // BodyLength counts from after "9=..|" up to and including the SOH before "10=".
    const auto body_start = msg.find(kSOH, msg.find("9=")) + 1;
    const auto cs_pos = msg.rfind("10=");
    const auto declared = std::stoul(msg.substr(msg.find("9=") + 2));
    EXPECT_EQ(declared, cs_pos - body_start);
    EXPECT_EQ(std::stoi(msg.substr(cs_pos + 3, 3)), checksum(std::string_view(msg).substr(0, cs_pos)));
    EXPECT_EQ(msg.find("35=0"), body_start) << "MsgType must be the first body field";
}

TEST(FixGateway, ForwardsCleanOrderAndPassesThroughSessionMessages) {
    RiskEngine e(cfg());
    FixGateway gw(e);
    const auto d = gw.on_outbound(nos("O1", "100", "150.00"), T0);
    EXPECT_TRUE(d.forward);
    EXPECT_EQ(d.msg_type, 'D');
    EXPECT_EQ(e.position("DESK1", "AAPL").open_buy_qty, 100);

    const auto hb = gw.on_outbound("35=0|", T0);
    EXPECT_TRUE(hb.forward);
    EXPECT_EQ(hb.msg_type, 0);
}

TEST(FixGateway, RejectBuildsExecutionReport) {
    RiskEngine e(cfg());
    FixGateway gw(e);
    const auto d = gw.on_outbound(nos("O1", "5000", "150.00"), T0);
    ASSERT_FALSE(d.forward);
    EXPECT_EQ(d.reason, RejectReason::MaxOrderQty);

    const FixView r(d.reject_fields);
    EXPECT_EQ(r.msg_type(), "8");
    EXPECT_EQ(r.get(11), "O1");
    EXPECT_EQ(r.get(150), "8");
    EXPECT_EQ(r.get(39), "8");
    EXPECT_EQ(r.get(103), "3");  // Order exceeds limit
    EXPECT_EQ(r.get(58), "RISK:MaxOrderQty");
    EXPECT_EQ(r.get(55), "AAPL");
}

TEST(FixGateway, UnsupportedOrderTypesFailClosed) {
    RiskEngine e(cfg());
    FixGateway gw(e);
    const auto stop = gw.on_outbound("35=D|11=S1|55=AAPL|54=1|38=10|40=3|99=149|", T0);
    EXPECT_FALSE(stop.forward);
    EXPECT_EQ(stop.reason, RejectReason::InvalidOrder);
    const auto bad_side = gw.on_outbound("35=D|11=S2|55=AAPL|54=9|38=10|40=2|44=150|", T0);
    EXPECT_FALSE(bad_side.forward);
}

TEST(FixGateway, ExecReportsDriveEngineState) {
    RiskEngine e(cfg());
    FixGateway gw(e);
    ASSERT_TRUE(gw.on_outbound(nos("O1", "300", "150.00"), T0).forward);

    EXPECT_TRUE(gw.on_inbound("35=8|11=O1|150=0|39=0|55=AAPL|"));
    // Partial fill with LastShares/LastPx.
    EXPECT_TRUE(gw.on_inbound("35=8|11=O1|150=1|39=1|32=100|31=150.10|14=100|151=200|"));
    EXPECT_EQ(e.position("DESK1", "AAPL").position, 100);
    EXPECT_EQ(e.reference_price("AAPL"), to_price(150.10));
    // Fill without LastShares: derived from the CumQty delta.
    EXPECT_TRUE(gw.on_inbound("35=8|11=O1|150=2|39=2|14=300|151=0|6=150.05|"));
    EXPECT_EQ(e.position("DESK1", "AAPL").position, 300);
    EXPECT_EQ(e.live_order_count(), 0u);
}

TEST(FixGateway, CancelReplaceRoundTrip) {
    RiskEngine e(cfg());
    FixGateway gw(e);
    ASSERT_TRUE(gw.on_outbound(nos("O1", "300", "150.00"), T0).forward);
    ASSERT_TRUE(gw.on_inbound("35=8|11=O1|150=0|39=0|"));

    // Replace up to 600 @ 151.
    ASSERT_TRUE(gw.on_outbound("35=G|11=O2|41=O1|55=AAPL|54=1|38=600|40=2|44=151.00|", T0).forward);
    EXPECT_EQ(e.position("DESK1", "AAPL").open_buy_qty, 600);
    ASSERT_TRUE(gw.on_inbound("35=8|11=O2|41=O1|150=5|39=0|"));

    // Replace rejected by the venue via OrderCancelReject (434=2).
    ASSERT_TRUE(gw.on_outbound("35=G|11=O3|41=O2|55=AAPL|54=1|38=900|40=2|44=151.00|", T0).forward);
    EXPECT_EQ(e.position("DESK1", "AAPL").open_buy_qty, 900);
    ASSERT_TRUE(gw.on_inbound("35=9|11=O3|41=O2|39=0|434=2|102=0|"));
    EXPECT_EQ(e.position("DESK1", "AAPL").open_buy_qty, 600);

    // Cancel and confirm.
    ASSERT_TRUE(gw.on_outbound("35=F|11=C1|41=O2|55=AAPL|54=1|", T0).forward);
    ASSERT_TRUE(gw.on_inbound("35=8|11=C1|41=O2|150=4|39=4|"));
    EXPECT_EQ(e.live_order_count(), 0u);
}

TEST(FixGateway, RejectedReplaceBuildsCancelReject) {
    RiskEngine e(cfg());
    FixGateway gw(e);
    const auto d = gw.on_outbound("35=G|11=O2|41=NOPE|55=AAPL|54=1|38=10|40=2|44=150.00|", T0);
    ASSERT_FALSE(d.forward);
    const FixView r(d.reject_fields);
    EXPECT_EQ(r.msg_type(), "9");
    EXPECT_EQ(r.get(434), "2");
    EXPECT_EQ(r.get(102), "1");  // Unknown order
    EXPECT_EQ(r.get(41), "NOPE");
}

TEST(FixGateway, VenueRejectOfNewOrderReleasesExposure) {
    RiskEngine e(cfg());
    FixGateway gw(e);
    ASSERT_TRUE(gw.on_outbound(nos("O1", "300", "150.00"), T0).forward);
    ASSERT_TRUE(gw.on_inbound("35=8|11=O1|150=8|39=8|58=venue says no|"));
    EXPECT_EQ(e.position("DESK1", "AAPL").open_buy_qty, 0);
}
