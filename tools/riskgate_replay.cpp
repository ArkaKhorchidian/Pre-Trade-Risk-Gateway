// riskgate_replay: run a FIX session log through the risk gateway.
//
// Client -> venue order messages (35=D/F/G) are risk checked; venue -> client
// execution reports (35=8/9) update positions. Prints one line per decision
// and a summary with positions and per-check latency.
//
// Accepts SOH or '|' delimited logs, one message per line; lines starting
// with '#' are ignored. Works directly on the logs produced by fix_gen /
// sample_session.txt in FIX-Protocol-Parser-Order-Router.
//
//   riskgate_replay [--config risk.ini] [--quiet] session.log

#include "riskgate/fix_gateway.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace riskgate;

namespace {

void usage() {
    std::fprintf(stderr, "usage: riskgate_replay [--config risk.ini] [--quiet] <session.log>\n");
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.remove_suffix(1);
    return s;
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path, log_path;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (a == "--quiet") quiet = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else log_path = a;
    }
    if (log_path.empty()) { usage(); return 2; }

    EngineConfig cfg;
    try {
        if (!config_path.empty()) {
            cfg = EngineConfig::from_file(config_path);
        } else {
            // Permissive defaults so a plain session log replays cleanly.
            cfg.symbol_defaults.require_reference_price = false;
            cfg.symbol_defaults.max_order_qty = 100'000;
            cfg.symbol_defaults.max_position = 1'000'000;
            cfg.symbol_defaults.price_band_bps = 1'000;
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "config error: %s\n", ex.what());
        return 2;
    }

    std::ifstream in(log_path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", log_path.c_str()); return 2; }

    RiskEngine engine(cfg);
    fix::FixGateway gw(engine);

    std::vector<double> lat_ns;
    std::set<std::pair<std::string, std::string>> seen_positions;
    std::uint64_t forwarded = 0, rejected = 0, inbound = 0;
    std::string line;
    Nanos clock = 1'000'000'000;  // logical clock: 1 ms per message

    while (std::getline(in, line)) {
        const std::string_view msg = trim(line);
        if (msg.empty() || msg.front() == '#') continue;
        const fix::FixView v(msg);
        const std::string_view mt = v.msg_type();
        clock += 1'000'000;

        if (mt == "D" || mt == "F" || mt == "G") {
            const auto t0 = std::chrono::steady_clock::now();
            const fix::Decision d = gw.on_outbound(msg, clock);
            const auto t1 = std::chrono::steady_clock::now();
            lat_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
            if (mt == "D") seen_positions.emplace(std::string(v.get(1)), std::string(v.get(55)));
            if (d.forward) ++forwarded; else ++rejected;
            if (!quiet) {
                std::printf("%-6s 35=%c ClOrdID=%-18.*s %s\n",
                            d.forward ? "PASS" : "REJECT", d.msg_type,
                            static_cast<int>(v.get(11).size()), v.get(11).data(),
                            d.forward ? "" : std::string(reject_reason_name(d.reason)).c_str());
            }
        } else if (mt == "8" || mt == "9") {
            ++inbound;
            (void)gw.on_inbound(msg);
        }
    }

    std::printf("\n── Summary ─────────────────────────────────────────\n");
    std::printf("order messages: %llu forwarded, %llu rejected; %llu execution reports applied\n",
                static_cast<unsigned long long>(forwarded), static_cast<unsigned long long>(rejected),
                static_cast<unsigned long long>(inbound));
    std::printf("kill switch: %s, live orders: %u\n",
                engine.kill_switch_engaged() ? "ENGAGED" : "off", engine.live_order_count());

    const auto& st = engine.stats();
    for (std::size_t i = 1; i < st.rejects_by_reason.size(); ++i) {
        if (st.rejects_by_reason[i] == 0) continue;
        std::printf("  %-22s %llu\n", std::string(reject_reason_name(static_cast<RejectReason>(i))).c_str(),
                    static_cast<unsigned long long>(st.rejects_by_reason[i]));
    }

    std::printf("\nPositions (account / symbol: position, open buy, open sell)\n");
    for (const auto& [acc, sym] : seen_positions) {
        const auto p = engine.position(acc, sym);
        std::printf("  %-10s %-8s %8lld %8lld %8lld\n", acc.empty() ? "(none)" : acc.c_str(), sym.c_str(),
                    static_cast<long long>(p.position), static_cast<long long>(p.open_buy_qty),
                    static_cast<long long>(p.open_sell_qty));
    }

    if (!lat_ns.empty()) {
        std::sort(lat_ns.begin(), lat_ns.end());
        auto pct = [&](double q) { return lat_ns[static_cast<std::size_t>(q * static_cast<double>(lat_ns.size() - 1))]; };
        std::printf("\nparse + check latency (ns, includes clock overhead): p50 %.0f  p99 %.0f  max %.0f\n",
                    pct(0.50), pct(0.99), lat_ns.back());
    }
    return 0;
}
