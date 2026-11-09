#pragma once

// Core value types for the pre-trade risk engine.
//
// Prices are fixed-point integers (1 unit = 1/10,000 of the quote currency),
// the same convention NASDAQ uses for OUCH/ITCH prices. Floating point never
// appears on the check path: a risk limit that fires at 999,999.9999999 on one
// box and 1,000,000.0000001 on another is not a limit.

#include <cstdint>
#include <string_view>

namespace riskgate {

using Price    = std::int64_t;   // fixed point, kPriceScale units per 1.0
using Qty      = std::int64_t;
using Notional = std::int64_t;   // Price * Qty, in kPriceScale units
using Nanos    = std::uint64_t;  // monotonic timestamp supplied by the caller

inline constexpr Price kPriceScale = 10'000;

using SymbolId  = std::uint32_t;
using AccountId = std::uint32_t;
using OrderSlot = std::uint32_t;

inline constexpr std::uint32_t kInvalidId = 0xFFFF'FFFFu;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrdType : std::uint8_t { Limit, Market };

[[nodiscard]] constexpr Price to_price(double px) noexcept {
    return static_cast<Price>(px * static_cast<double>(kPriceScale) + (px >= 0 ? 0.5 : -0.5));
}

[[nodiscard]] constexpr double to_double(Price px) noexcept {
    return static_cast<double>(px) / static_cast<double>(kPriceScale);
}

// Every rejection carries exactly one reason. Order matters only for the
// documentation table in the README; the engine evaluates checks in the order
// listed in RiskEngine::check_new().
enum class RejectReason : std::uint8_t {
    None = 0,
    KillSwitch,
    InvalidOrder,
    ClOrdIdTooLong,
    DuplicateClOrdId,
    UnknownSymbol,
    UnknownAccount,
    RestrictedSymbol,
    MarketOrdersDisabled,
    NoReferencePrice,
    PriceOutsideBand,
    MaxOrderQty,
    MaxOrderNotional,
    MaxPosition,
    MaxOpenNotional,
    MaxOpenOrders,
    RateLimited,
    SelfTrade,
    UnknownOrder,
    OrderNotOpen,
    CapacityExceeded,
};

[[nodiscard]] constexpr std::string_view reject_reason_name(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None:                 return "None";
        case RejectReason::KillSwitch:           return "KillSwitch";
        case RejectReason::InvalidOrder:         return "InvalidOrder";
        case RejectReason::ClOrdIdTooLong:       return "ClOrdIdTooLong";
        case RejectReason::DuplicateClOrdId:     return "DuplicateClOrdId";
        case RejectReason::UnknownSymbol:        return "UnknownSymbol";
        case RejectReason::UnknownAccount:       return "UnknownAccount";
        case RejectReason::RestrictedSymbol:     return "RestrictedSymbol";
        case RejectReason::MarketOrdersDisabled: return "MarketOrdersDisabled";
        case RejectReason::NoReferencePrice:     return "NoReferencePrice";
        case RejectReason::PriceOutsideBand:     return "PriceOutsideBand";
        case RejectReason::MaxOrderQty:          return "MaxOrderQty";
        case RejectReason::MaxOrderNotional:     return "MaxOrderNotional";
        case RejectReason::MaxPosition:          return "MaxPosition";
        case RejectReason::MaxOpenNotional:      return "MaxOpenNotional";
        case RejectReason::MaxOpenOrders:        return "MaxOpenOrders";
        case RejectReason::RateLimited:          return "RateLimited";
        case RejectReason::SelfTrade:            return "SelfTrade";
        case RejectReason::UnknownOrder:         return "UnknownOrder";
        case RejectReason::OrderNotOpen:         return "OrderNotOpen";
        case RejectReason::CapacityExceeded:     return "CapacityExceeded";
    }
    return "Unknown";
}

struct CheckResult {
    RejectReason reason{RejectReason::None};

    [[nodiscard]] constexpr bool accepted() const noexcept { return reason == RejectReason::None; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return accepted(); }

    static constexpr CheckResult ok() noexcept { return {}; }
    static constexpr CheckResult reject(RejectReason r) noexcept { return {r}; }
};

// Order intent as seen by the engine. String fields are views: the engine
// copies what it needs into fixed-size inline storage and never keeps them.
struct NewOrderRequest {
    std::string_view cl_ord_id;
    std::string_view account;
    std::string_view symbol;
    Side             side{Side::Buy};
    OrdType          type{OrdType::Limit};
    Qty              qty{0};
    Price            price{0};   // ignored for market orders
};

struct ReplaceRequest {
    std::string_view cl_ord_id;       // new ClOrdID
    std::string_view orig_cl_ord_id;  // order being replaced
    Qty              qty{0};          // new total order quantity (FIX semantics)
    Price            price{0};
};

struct CancelRequest {
    std::string_view cl_ord_id;
    std::string_view orig_cl_ord_id;
};

} // namespace riskgate
