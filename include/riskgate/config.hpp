#pragma once

// Risk limit configuration and a small INI loader.
//
// Limits are set per symbol and per account, each falling back to a defaults
// section. The loader runs at startup only; nothing here is on the hot path.

#include "types.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace riskgate {

struct SymbolLimits {
    Qty      max_order_qty{0};            // 0 = no limit
    Notional max_order_notional{0};       // 0 = no limit (kPriceScale units)
    Qty      max_position{0};             // absolute, worst case incl. open orders
    std::uint32_t price_band_bps{0};      // 0 = no collar
    bool     require_reference_price{true};
    bool     restricted{false};
    Price    reference_price{0};          // initial reference; 0 = none yet
};

struct AccountLimits {
    std::uint32_t max_open_orders{0};     // 0 = no limit
    Notional      max_open_notional{0};   // 0 = no limit
    std::uint32_t orders_per_second{0};   // 0 = unlimited
    std::uint32_t burst{1};
};

struct EngineConfig {
    std::uint32_t max_orders{65'536};          // live orders tracked at once
    std::uint32_t max_cl_ord_ids{1u << 20};    // ClOrdIDs remembered for dup detection
    std::uint32_t max_symbols{4'096};
    std::uint32_t max_accounts{256};
    std::uint32_t max_positions{65'536};       // distinct (account, symbol) pairs
    bool allow_unconfigured_symbols{true};     // use symbol_defaults
    bool allow_unconfigured_accounts{true};    // use account_defaults
    bool allow_market_orders{false};
    bool fills_update_reference{true};
    bool self_trade_prevention{true};

    // Auto kill switch: trip if this many risk rejects happen within the window.
    std::uint32_t kill_switch_reject_threshold{0};  // 0 = disabled
    std::uint64_t kill_switch_window_ns{1'000'000'000ULL};

    SymbolLimits  symbol_defaults{};
    AccountLimits account_defaults{};
    std::map<std::string, SymbolLimits, std::less<>>  symbols;
    std::map<std::string, AccountLimits, std::less<>> accounts;

    [[nodiscard]] static EngineConfig from_ini(std::istream& in);
    [[nodiscard]] static EngineConfig from_file(const std::string& path);
};

// ── INI loader ────────────────────────────────────────────────────────────────

namespace detail {

inline std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline bool parse_bool(const std::string& v) {
    if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    throw std::runtime_error("invalid boolean: " + v);
}

template <class T>
T parse_uint(const std::string& v) {
    T out{};
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    if (ec != std::errc{} || p != v.data() + v.size())
        throw std::runtime_error("invalid integer: " + v);
    return out;
}

// Decimal price/notional string -> fixed point, exact (no double round trip).
inline std::int64_t parse_fixed(const std::string& v) {
    std::int64_t whole = 0, frac = 0, scale = kPriceScale;
    bool neg = false, seen_dot = false, any = false;
    for (std::size_t i = 0; i < v.size(); ++i) {
        const char c = v[i];
        if (c == '-' && i == 0) { neg = true; continue; }
        if (c == '_' || c == ',') continue;  // allow 1_000_000
        if (c == '.') {
            if (seen_dot) throw std::runtime_error("invalid number: " + v);
            seen_dot = true;
            continue;
        }
        if (c < '0' || c > '9') throw std::runtime_error("invalid number: " + v);
        any = true;
        if (!seen_dot) {
            whole = whole * 10 + (c - '0');
        } else if (scale > 1) {
            scale /= 10;
            frac += (c - '0') * scale;
        }
    }
    if (!any) throw std::runtime_error("invalid number: " + v);
    const std::int64_t r = whole * kPriceScale + frac;
    return neg ? -r : r;
}

inline void apply_symbol_key(SymbolLimits& s, const std::string& k, const std::string& v) {
    if      (k == "max_order_qty")           s.max_order_qty = parse_uint<Qty>(v);
    else if (k == "max_order_notional")      s.max_order_notional = parse_fixed(v);
    else if (k == "max_position")            s.max_position = parse_uint<Qty>(v);
    else if (k == "price_band_bps")          s.price_band_bps = parse_uint<std::uint32_t>(v);
    else if (k == "require_reference_price") s.require_reference_price = parse_bool(v);
    else if (k == "restricted")              s.restricted = parse_bool(v);
    else if (k == "reference_price")         s.reference_price = parse_fixed(v);
    else throw std::runtime_error("unknown symbol key: " + k);
}

inline void apply_account_key(AccountLimits& a, const std::string& k, const std::string& v) {
    if      (k == "max_open_orders")   a.max_open_orders = parse_uint<std::uint32_t>(v);
    else if (k == "max_open_notional") a.max_open_notional = parse_fixed(v);
    else if (k == "orders_per_second") a.orders_per_second = parse_uint<std::uint32_t>(v);
    else if (k == "burst")             a.burst = parse_uint<std::uint32_t>(v);
    else throw std::runtime_error("unknown account key: " + k);
}

inline void apply_engine_key(EngineConfig& c, const std::string& k, const std::string& v) {
    if      (k == "max_orders")                   c.max_orders = parse_uint<std::uint32_t>(v);
    else if (k == "max_cl_ord_ids")               c.max_cl_ord_ids = parse_uint<std::uint32_t>(v);
    else if (k == "max_symbols")                  c.max_symbols = parse_uint<std::uint32_t>(v);
    else if (k == "max_accounts")                 c.max_accounts = parse_uint<std::uint32_t>(v);
    else if (k == "max_positions")                c.max_positions = parse_uint<std::uint32_t>(v);
    else if (k == "allow_unconfigured_symbols")   c.allow_unconfigured_symbols = parse_bool(v);
    else if (k == "allow_unconfigured_accounts")  c.allow_unconfigured_accounts = parse_bool(v);
    else if (k == "allow_market_orders")          c.allow_market_orders = parse_bool(v);
    else if (k == "fills_update_reference")       c.fills_update_reference = parse_bool(v);
    else if (k == "self_trade_prevention")        c.self_trade_prevention = parse_bool(v);
    else if (k == "kill_switch_reject_threshold") c.kill_switch_reject_threshold = parse_uint<std::uint32_t>(v);
    else if (k == "kill_switch_window_ms")
        c.kill_switch_window_ns = parse_uint<std::uint64_t>(v) * 1'000'000ULL;
    else throw std::runtime_error("unknown engine key: " + k);
}

} // namespace detail

// Format:
//   [engine]            engine-wide settings
//   [symbol_defaults]   limits for any symbol without its own section
//   [symbol AAPL]       per-symbol overrides (start from symbol_defaults)
//   [account_defaults]
//   [account DESK1]
// Lines starting with # or ; are comments.
inline EngineConfig EngineConfig::from_ini(std::istream& in) {
    EngineConfig cfg;
    enum class Sec { None, Engine, SymDefaults, Symbol, AccDefaults, Account } sec = Sec::None;
    std::string name;
    std::string line;
    int lineno = 0;

    // Per-symbol sections must start from the defaults even if the defaults
    // section appears later in the file, so collect raw key/values first.
    std::map<std::string, std::map<std::string, std::string>> sym_raw, acc_raw;

    while (std::getline(in, line)) {
        ++lineno;
        line = detail::trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        try {
            if (line.front() == '[') {
                if (line.back() != ']') throw std::runtime_error("unterminated section");
                std::string hdr = detail::trim(line.substr(1, line.size() - 2));
                const auto sp = hdr.find(' ');
                const std::string kind = sp == std::string::npos ? hdr : hdr.substr(0, sp);
                name = sp == std::string::npos ? "" : detail::trim(hdr.substr(sp + 1));
                if      (kind == "engine")           sec = Sec::Engine;
                else if (kind == "symbol_defaults")  sec = Sec::SymDefaults;
                else if (kind == "account_defaults") sec = Sec::AccDefaults;
                else if (kind == "symbol" && !name.empty())  { sec = Sec::Symbol;  sym_raw[name]; }
                else if (kind == "account" && !name.empty()) { sec = Sec::Account; acc_raw[name]; }
                else throw std::runtime_error("unknown section: " + hdr);
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) throw std::runtime_error("expected key = value");
            const std::string k = detail::trim(line.substr(0, eq));
            std::string v = detail::trim(line.substr(eq + 1));
            if (const auto hash = v.find(" #"); hash != std::string::npos) v = detail::trim(v.substr(0, hash));
            switch (sec) {
                case Sec::Engine:      detail::apply_engine_key(cfg, k, v); break;
                case Sec::SymDefaults: detail::apply_symbol_key(cfg.symbol_defaults, k, v); break;
                case Sec::AccDefaults: detail::apply_account_key(cfg.account_defaults, k, v); break;
                case Sec::Symbol:      sym_raw[name][k] = v; break;
                case Sec::Account:     acc_raw[name][k] = v; break;
                case Sec::None:        throw std::runtime_error("key outside of a section");
            }
        } catch (const std::exception& e) {
            throw std::runtime_error("config line " + std::to_string(lineno) + ": " + e.what());
        }
    }

    for (auto& [sym, kv] : sym_raw) {
        SymbolLimits s = cfg.symbol_defaults;
        for (auto& [k, v] : kv) detail::apply_symbol_key(s, k, v);
        cfg.symbols[sym] = s;
    }
    for (auto& [acc, kv] : acc_raw) {
        AccountLimits a = cfg.account_defaults;
        for (auto& [k, v] : kv) detail::apply_account_key(a, k, v);
        cfg.accounts[acc] = a;
    }
    return cfg;
}

inline EngineConfig EngineConfig::from_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open config: " + path);
    return from_ini(f);
}

} // namespace riskgate
