#pragma once

// FIX 4.2 adapter for the risk engine.
//
// FixGateway sits on the order path between a FIX client (OMS / strategy) and
// the venue session:
//
//   client ──D/F/G──► FixGateway::on_outbound() ──forward──► venue
//                            │
//                            └──reject──► ExecutionReport / OrderCancelReject
//                                         back to the client
//
//   venue ──8/9──► FixGateway::on_inbound() ──► engine state update
//
// The gateway works on application-level fields only. Session concerns
// (BodyLength, CheckSum validation, sequence numbers, heartbeats) belong to
// the session layer that owns the socket; frame_fix() is provided for tools
// and tests that need a complete wire message.

#include "risk_engine.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace riskgate::fix {

inline constexpr char kSOH = '\x01';

// ── Zero-copy tag=value view ──────────────────────────────────────────────────

class FixView {
public:
    static constexpr std::size_t kMaxFields = 96;

    // Accepts SOH or '|' as the field delimiter (logs usually use '|').
    explicit FixView(std::string_view msg) noexcept {
        std::size_t i = 0;
        while (i < msg.size() && count_ < kMaxFields) {
            std::size_t end = i;
            while (end < msg.size() && msg[end] != kSOH && msg[end] != '|') ++end;
            const std::string_view field = msg.substr(i, end - i);
            if (const auto eq = field.find('='); eq != std::string_view::npos && eq > 0) {
                int tag = 0;
                auto [p, ec] = std::from_chars(field.data(), field.data() + eq, tag);
                if (ec == std::errc{} && p == field.data() + eq) {
                    fields_[count_++] = {tag, field.substr(eq + 1)};
                }
            }
            i = end + 1;
        }
    }

    [[nodiscard]] std::string_view get(int tag) const noexcept {
        for (std::size_t i = 0; i < count_; ++i)
            if (fields_[i].first == tag) return fields_[i].second;
        return {};
    }
    [[nodiscard]] bool has(int tag) const noexcept {
        for (std::size_t i = 0; i < count_; ++i)
            if (fields_[i].first == tag) return true;
        return false;
    }
    [[nodiscard]] std::string_view msg_type() const noexcept { return get(35); }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    std::array<std::pair<int, std::string_view>, kMaxFields> fields_{};
    std::size_t count_{0};
};

// Exact decimal -> fixed point (no double). Returns false on malformed input.
[[nodiscard]] bool parse_price(std::string_view s, Price& out) noexcept;
[[nodiscard]] bool parse_qty(std::string_view s, Qty& out) noexcept;
[[nodiscard]] std::string format_price(Price px);

// FIX 4.2 OrdRejReason (103) and CxlRejReason (102) for a risk reject.
[[nodiscard]] int ord_rej_reason(RejectReason r) noexcept;
[[nodiscard]] int cxl_rej_reason(RejectReason r) noexcept;

// Wrap application fields (starting at 35=) with header and trailer:
// 8, 9 (BodyLength), 35.., 49, 56, 34, 52 ... 10 (CheckSum).
[[nodiscard]] std::string frame_fix(std::string_view begin_string,
                                    std::string_view sender_comp_id,
                                    std::string_view target_comp_id,
                                    std::uint32_t    seq_num,
                                    std::string_view sending_time,
                                    std::string_view app_fields);

[[nodiscard]] std::uint8_t checksum(std::string_view bytes) noexcept;

// ── Gateway ───────────────────────────────────────────────────────────────────

struct Decision {
    bool         forward{true};        // send the original message to the venue
    RejectReason reason{RejectReason::None};
    char         msg_type{0};          // 'D', 'F', 'G', or 0 for pass-through
    std::string  reject_fields;        // app fields (35=8 / 35=9 ...) when rejected
};

class FixGateway {
public:
    explicit FixGateway(RiskEngine& engine) : engine_(engine) {}

    // Client -> venue. Non-order messages pass through untouched.
    [[nodiscard]] Decision on_outbound(std::string_view msg, Nanos now);

    // Venue -> client. Returns true if the message changed engine state.
    bool on_inbound(std::string_view msg);

    [[nodiscard]] RiskEngine& engine() noexcept { return engine_; }

private:
    std::string build_order_reject(const FixView& v, RejectReason r);
    std::string build_cancel_reject(const FixView& v, char response_to, RejectReason r);

    RiskEngine&   engine_;
    std::uint64_t exec_id_seq_{0};
};

} // namespace riskgate::fix
