#include "riskgate/fix_gateway.hpp"

#include <cstdio>
#include <string>

namespace riskgate::fix {

namespace tag {
inline constexpr int Account = 1, AvgPx = 6, ClOrdID = 11, CumQty = 14, ExecID = 17,
                     ExecTransType = 20, LastPx = 31, LastShares = 32, OrderID = 37,
                     OrderQty = 38, OrdStatus = 39, OrdType = 40, OrigClOrdID = 41,
                     Price = 44, Side = 54, Symbol = 55, Text = 58, LeavesQty = 151,
                     CxlRejReason = 102, OrdRejReason = 103, ExecType = 150,
                     CxlRejResponseTo = 434;
}

// ── Number parsing / formatting ───────────────────────────────────────────────

bool parse_price(std::string_view s, Price& out) noexcept {
    if (s.empty()) return false;
    std::int64_t whole = 0, frac = 0, scale = kPriceScale;
    bool neg = false, dot = false, digits = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '-' && i == 0) { neg = true; continue; }
        if (c == '.') { if (dot) return false; dot = true; continue; }
        if (c < '0' || c > '9') return false;
        digits = true;
        if (!dot) {
            if (whole > (INT64_MAX / kPriceScale) / 10) return false;
            whole = whole * 10 + (c - '0');
        } else if (scale > 1) {
            scale /= 10;
            frac += (c - '0') * scale;
        }
        // Digits beyond 4 decimal places are truncated: venues reject
        // sub-tick prices anyway, and the risk check must not round up.
    }
    if (!digits) return false;
    out = (whole * kPriceScale + frac) * (neg ? -1 : 1);
    return true;
}

bool parse_qty(std::string_view s, Qty& out) noexcept {
    // FIX 4.2 Qty is a float type; accept "100" and "100.0" but not fractions.
    if (const auto dot = s.find('.'); dot != std::string_view::npos) {
        for (char c : s.substr(dot + 1)) if (c != '0') return false;
        s = s.substr(0, dot);
    }
    if (s.empty()) return false;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}

std::string format_price(Price px) {
    char buf[32];
    const bool neg = px < 0;
    const std::uint64_t a = neg ? static_cast<std::uint64_t>(-px) : static_cast<std::uint64_t>(px);
    std::snprintf(buf, sizeof buf, "%s%llu.%04llu", neg ? "-" : "",
                  static_cast<unsigned long long>(a / kPriceScale),
                  static_cast<unsigned long long>(a % kPriceScale));
    return buf;
}

int ord_rej_reason(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::UnknownSymbol:    return 1;  // Unknown symbol
        case RejectReason::DuplicateClOrdId: return 6;  // Duplicate order
        case RejectReason::UnknownOrder:     return 5;  // Unknown order
        case RejectReason::PriceOutsideBand:
        case RejectReason::MaxOrderQty:
        case RejectReason::MaxOrderNotional:
        case RejectReason::MaxPosition:
        case RejectReason::MaxOpenNotional:
        case RejectReason::MaxOpenOrders:    return 3;  // Order exceeds limit
        default:                             return 0;  // Broker option
    }
}

int cxl_rej_reason(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::UnknownOrder: return 1;  // Unknown order
        case RejectReason::OrderNotOpen: return 3;  // Order already in pending status
        default:                         return 2;  // Broker option
    }
}

std::uint8_t checksum(std::string_view bytes) noexcept {
    unsigned sum = 0;
    for (unsigned char c : bytes) sum += c;
    return static_cast<std::uint8_t>(sum % 256);
}

std::string frame_fix(std::string_view begin_string, std::string_view sender,
                      std::string_view target, std::uint32_t seq,
                      std::string_view sending_time, std::string_view app_fields) {
    // Header fields go after MsgType (35), which must be the first body field.
    std::string_view msg_type_field, rest = app_fields;
    if (const auto soh = app_fields.find(kSOH); soh != std::string_view::npos) {
        msg_type_field = app_fields.substr(0, soh + 1);
        rest = app_fields.substr(soh + 1);
    }
    std::string body;
    body.reserve(app_fields.size() + 64);
    body.append(msg_type_field);
    body.append("49=").append(sender).push_back(kSOH);
    body.append("56=").append(target).push_back(kSOH);
    body.append("34=").append(std::to_string(seq)).push_back(kSOH);
    body.append("52=").append(sending_time).push_back(kSOH);
    body.append(rest);

    std::string out;
    out.reserve(body.size() + 32);
    out.append("8=").append(begin_string).push_back(kSOH);
    out.append("9=").append(std::to_string(body.size())).push_back(kSOH);
    out.append(body);
    char cs[8];
    std::snprintf(cs, sizeof cs, "%03u", checksum(out));
    out.append("10=").append(cs).push_back(kSOH);
    return out;
}

// ── Reject builders (reject path only: allocation is fine here) ───────────────

namespace {
void add(std::string& s, int t, std::string_view v) {
    s.append(std::to_string(t)).push_back('=');
    s.append(v).push_back(kSOH);
}
} // namespace

std::string FixGateway::build_order_reject(const FixView& v, RejectReason r) {
    std::string s;
    s.reserve(256);
    add(s, 35, "8");
    add(s, tag::OrderID, "NONE");
    add(s, tag::ClOrdID, v.get(tag::ClOrdID));
    add(s, tag::ExecID, "RISK-" + std::to_string(++exec_id_seq_));
    add(s, tag::ExecTransType, "0");
    add(s, tag::ExecType, "8");
    add(s, tag::OrdStatus, "8");
    if (v.has(tag::Account)) add(s, tag::Account, v.get(tag::Account));
    add(s, tag::Symbol, v.get(tag::Symbol));
    add(s, tag::Side, v.get(tag::Side));
    add(s, tag::OrderQty, v.get(tag::OrderQty));
    add(s, tag::LeavesQty, "0");
    add(s, tag::CumQty, "0");
    add(s, tag::AvgPx, "0");
    add(s, tag::OrdRejReason, std::to_string(ord_rej_reason(r)));
    add(s, tag::Text, std::string("RISK:") + std::string(reject_reason_name(r)));
    return s;
}

std::string FixGateway::build_cancel_reject(const FixView& v, char response_to, RejectReason r) {
    std::string s;
    s.reserve(192);
    add(s, 35, "9");
    add(s, tag::OrderID, "NONE");
    add(s, tag::ClOrdID, v.get(tag::ClOrdID));
    add(s, tag::OrigClOrdID, v.get(tag::OrigClOrdID));
    // Status of the original order after the reject: still live unless unknown.
    add(s, tag::OrdStatus, r == RejectReason::UnknownOrder ? "8" : "0");
    add(s, tag::CxlRejResponseTo, std::string_view(&response_to, 1));
    add(s, tag::CxlRejReason, std::to_string(cxl_rej_reason(r)));
    add(s, tag::Text, std::string("RISK:") + std::string(reject_reason_name(r)));
    return s;
}

// ── Outbound (client -> venue) ────────────────────────────────────────────────

Decision FixGateway::on_outbound(std::string_view msg, Nanos now) {
    const FixView v(msg);
    const std::string_view mt = v.msg_type();
    Decision d;
    if (mt.size() != 1) return d;  // session / unknown message: pass through
    d.msg_type = mt[0];

    switch (mt[0]) {
        case 'D': {  // NewOrderSingle
            NewOrderRequest req;
            req.cl_ord_id = v.get(tag::ClOrdID);
            req.account   = v.get(tag::Account);
            req.symbol    = v.get(tag::Symbol);
            const std::string_view side = v.get(tag::Side);
            const std::string_view ot   = v.get(tag::OrdType);
            bool ok = side.size() == 1 && ot.size() == 1 && parse_qty(v.get(tag::OrderQty), req.qty);
            if (ok) {
                // 1=Buy; 2=Sell, 5=Sell short, 6=Sell short exempt.
                if (side[0] == '1') req.side = Side::Buy;
                else if (side[0] == '2' || side[0] == '5' || side[0] == '6') req.side = Side::Sell;
                else ok = false;
                // Only Market (1) and Limit (2). Stops etc. fail closed.
                if (ot[0] == '1') req.type = OrdType::Market;
                else if (ot[0] == '2') { req.type = OrdType::Limit; ok = ok && parse_price(v.get(tag::Price), req.price); }
                else ok = false;
            }
            const CheckResult r = ok ? engine_.check_new(req, now)
                                     : CheckResult::reject(RejectReason::InvalidOrder);
            if (!r) {
                d.forward = false;
                d.reason  = r.reason;
                d.reject_fields = build_order_reject(v, r.reason);
            }
            return d;
        }
        case 'G': {  // OrderCancelReplaceRequest
            ReplaceRequest req;
            req.cl_ord_id      = v.get(tag::ClOrdID);
            req.orig_cl_ord_id = v.get(tag::OrigClOrdID);
            const bool ok = parse_qty(v.get(tag::OrderQty), req.qty) &&
                            parse_price(v.get(tag::Price), req.price);
            const CheckResult r = ok ? engine_.check_replace(req, now)
                                     : CheckResult::reject(RejectReason::InvalidOrder);
            if (!r) {
                d.forward = false;
                d.reason  = r.reason;
                d.reject_fields = build_cancel_reject(v, '2', r.reason);
            }
            return d;
        }
        case 'F': {  // OrderCancelRequest
            const CheckResult r = engine_.check_cancel(
                {v.get(tag::ClOrdID), v.get(tag::OrigClOrdID)}, now);
            if (!r) {
                d.forward = false;
                d.reason  = r.reason;
                d.reject_fields = build_cancel_reject(v, '1', r.reason);
            }
            return d;
        }
        default:
            d.msg_type = 0;
            return d;
    }
}

// ── Inbound (venue -> client) ─────────────────────────────────────────────────

bool FixGateway::on_inbound(std::string_view msg) {
    const FixView v(msg);
    const std::string_view mt = v.msg_type();
    const std::string_view id = v.get(tag::ClOrdID);

    if (mt == "9") {  // OrderCancelReject
        const std::string_view to = v.get(tag::CxlRejResponseTo);
        if (to == "2") return engine_.on_replace_rejected(id);
        return engine_.on_cancel_rejected(id);
    }
    if (mt != "8") return false;

    const std::string_view et = v.get(tag::ExecType);
    if (et.size() != 1) return false;

    switch (et[0]) {
        case '0':  // New
            return engine_.on_new_ack(id);
        case '1':  // Partial fill
        case '2':  // Fill
        case 'F': {  // Trade (FIX 4.4+)
            Qty last = 0;
            if (!parse_qty(v.get(tag::LastShares), last) || last <= 0) {
                // No LastShares: derive the fill from the CumQty delta.
                Qty cum = 0;
                const auto prev = engine_.cum_qty(id);
                if (!prev || !parse_qty(v.get(tag::CumQty), cum)) return false;
                last = cum - *prev;
                if (last <= 0) return false;
            }
            Price px = 0;
            if (!parse_price(v.get(tag::LastPx), px) || px <= 0) {
                if (!parse_price(v.get(tag::AvgPx), px) || px <= 0)
                    (void)parse_price(v.get(tag::Price), px);
            }
            return engine_.on_fill(id, last, px);
        }
        case '5':  // Replaced
            return engine_.on_replaced(id);
        case '4':  // Canceled
        case '3':  // Done for day
        case 'C':  // Expired
            if (engine_.on_cancelled(id)) return true;
            return engine_.on_cancelled(v.get(tag::OrigClOrdID));
        case '8':  // Rejected
            if (engine_.is_pending_replace_id(id)) return engine_.on_replace_rejected(id);
            return engine_.on_order_rejected(id);
        default:
            return false;
    }
}

} // namespace riskgate::fix
