// riskgate_bench: latency of a pre-trade check under a realistic book.
//
// Workload: 500 symbols, 8 accounts, every limit enabled. Orders are pre-built
// so the timed region contains only the engine call. Live orders are kept at a
// steady state of ~2,000 by acking and then filling or cancelling older
// orders, so the tables are warm and partially full, as in production.
//
// Each check is timed individually with the TSC-backed steady_clock. The cost
// of the clock pair itself is measured and reported so it can be subtracted.

#include "riskgate/fix_gateway.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using namespace riskgate;
using Clock = std::chrono::steady_clock;

namespace {

struct Rng {
    std::uint64_t s = 0x9E3779B97F4A7C15ULL;
    std::uint64_t operator()() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
};

void report(const char* name, std::vector<double>& ns, double overhead) {
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double q) {
        return std::max(0.0, ns[static_cast<std::size_t>(q * static_cast<double>(ns.size() - 1))] - overhead);
    };
    std::printf("%-28s n=%-8zu p50 %6.0f  p90 %6.0f  p99 %6.0f  p99.9 %6.0f  max %8.0f ns\n",
                name, ns.size(), pct(0.50), pct(0.90), pct(0.99), pct(0.999), ns.back() - overhead);
}

double clock_overhead() {
    std::vector<double> v(200'000);
    for (auto& x : v) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        x = std::chrono::duration<double, std::nano>(b - a).count();
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

EngineConfig bench_config() {
    EngineConfig c;
    c.max_orders = 65'536;
    c.max_cl_ord_ids = 1'500'000;
    c.fills_update_reference = false;  // keep every order inside the collar
    c.symbol_defaults.max_order_qty = 10'000;
    c.symbol_defaults.max_order_notional = to_price(5'000'000.0);
    c.symbol_defaults.max_position = 10'000'000;
    c.symbol_defaults.price_band_bps = 500;
    c.symbol_defaults.reference_price = to_price(100.0);
    c.account_defaults.max_open_orders = 100'000;
    c.account_defaults.max_open_notional = to_price(1e12);
    c.account_defaults.orders_per_second = 1'000'000'000;  // enabled, never binding
    c.account_defaults.burst = 1'000'000;
    c.self_trade_prevention = true;
    return c;
}

} // namespace

int main() {
    constexpr int kSymbols = 500, kAccounts = 8, kOrders = 1'000'000, kLive = 2'000;
    const double overhead = clock_overhead();
    std::printf("clock pair overhead: %.0f ns (subtracted below)\n\n", overhead);

    std::vector<std::string> symbols, accounts;
    for (int i = 0; i < kSymbols; ++i) symbols.push_back("SYM" + std::to_string(i));
    for (int i = 0; i < kAccounts; ++i) accounts.push_back("ACC" + std::to_string(i));

    // ── Engine-only: check_new ────────────────────────────────────────────────
    {
        RiskEngine e(bench_config());
        Rng rng;
        std::vector<std::string> ids(kOrders);
        std::vector<NewOrderRequest> reqs(kOrders);
        for (int i = 0; i < kOrders; ++i) {
            ids[i] = "ORD" + std::to_string(i);
            // Buys below 100, sells above: no self-crossing, so all pass.
            const bool is_buy = rng() % 2;
            const Price px = is_buy ? to_price(99.0) - static_cast<Price>(rng() % 30'000)
                                    : to_price(101.0) + static_cast<Price>(rng() % 30'000);
            reqs[i] = {ids[i], accounts[rng() % kAccounts], symbols[rng() % kSymbols],
                       is_buy ? Side::Buy : Side::Sell, OrdType::Limit,
                       1 + static_cast<Qty>(rng() % 500), px};
        }

        // Warm up the symbol/account/position tables.
        Nanos now = 1'000'000'000;
        std::deque<int> live;
        std::vector<double> lat;
        lat.reserve(kOrders);
        std::uint64_t accepted = 0;

        for (int i = 0; i < kOrders; ++i) {
            now += 1'000;
            const auto t0 = Clock::now();
            const CheckResult r = e.check_new(reqs[i], now);
            const auto t1 = Clock::now();
            lat.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
            if (r) {
                ++accepted;
                (void)e.on_new_ack(ids[i]);
                live.push_back(i);
            }
            while (static_cast<int>(live.size()) > kLive) {
                const int old = live.front();
                live.pop_front();
                if (old % 3 == 0) (void)e.on_fill(ids[old], reqs[old].qty, reqs[old].price);
                else (void)e.on_cancelled(ids[old]);
            }
        }
        std::printf("accepted %llu / %d orders, live at end %u\n",
                    static_cast<unsigned long long>(accepted), kOrders, e.live_order_count());
        report("RiskEngine::check_new", lat, overhead);
    }

    // ── End to end: FIX parse + check + (no) reject build ─────────────────────
    {
        RiskEngine e(bench_config());
        fix::FixGateway gw(e);
        Rng rng;
        constexpr int kMsgs = 300'000;
        std::vector<std::string> msgs(kMsgs), ids(kMsgs);
        for (int i = 0; i < kMsgs; ++i) {
            ids[i] = "F" + std::to_string(i);
            const bool is_buy = rng() % 2;
            const double px = is_buy ? 99.0 - static_cast<double>(rng() % 300) / 100.0
                                     : 101.0 + static_cast<double>(rng() % 300) / 100.0;
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.2f", px);
            msgs[i] = "8=FIX.4.2|9=150|35=D|49=OMS|56=GW|34=" + std::to_string(i + 2) +
                      "|52=20260101-09:30:00.000|11=" + ids[i] + "|1=" + accounts[rng() % kAccounts] +
                      "|55=" + symbols[rng() % kSymbols] + "|54=" + (is_buy ? "1" : "2") +
                      "|60=20260101-09:30:00.000|38=" + std::to_string(1 + rng() % 500) +
                      "|40=2|44=" + buf + "|59=0|10=000|";
        }
        Nanos now = 1'000'000'000;
        std::deque<int> live;
        std::vector<double> lat;
        lat.reserve(kMsgs);
        for (int i = 0; i < kMsgs; ++i) {
            now += 1'000;
            const auto t0 = Clock::now();
            const fix::Decision d = gw.on_outbound(msgs[i], now);
            const auto t1 = Clock::now();
            lat.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
            if (d.forward) { (void)e.on_new_ack(ids[i]); live.push_back(i); }
            while (static_cast<int>(live.size()) > kLive) {
                (void)e.on_cancelled(ids[live.front()]);
                live.pop_front();
            }
        }
        report("FixGateway::on_outbound (D)", lat, overhead);
    }
    return 0;
}
