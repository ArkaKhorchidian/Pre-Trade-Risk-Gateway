#pragma once

// Pre-trade risk engine.
//
// Sits between the strategy / OMS and the order gateway. Every outbound order
// message is checked synchronously before it is written to the venue socket;
// every inbound execution report updates the engine's view of open orders and
// positions.
//
// Design rules:
//   * No allocation after construction. All tables are fixed capacity and the
//     engine fails closed (rejects) when one is full.
//   * No floating point on the check path. Prices and notionals are integers.
//   * Exposure is worst case. Open orders count against position and notional
//     limits as if they will fully fill. While a replace is in flight, the
//     order reserves max(old, new) exposure until the venue confirms.
//   * Cancels are never blocked. Not by the kill switch, not by the throttle.
//     Blocking a cancel converts a risk control into a risk.
//   * Single threaded. One engine per gateway thread; shard by account if you
//     need more. No locks, no atomics on the hot path.

#include "config.hpp"
#include "containers.hpp"
#include "token_bucket.hpp"
#include "types.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace riskgate {

inline constexpr std::size_t kMaxClOrdIdLen = 32;
inline constexpr std::size_t kMaxSymbolLen  = 16;
inline constexpr std::size_t kMaxAccountLen = 16;

using ClOrdId = FixedString<kMaxClOrdIdLen>;

enum class OrderState : std::uint8_t { PendingNew, Open, PendingReplace };

// Read-only snapshot of an order, for kill-switch cancel sweeps and tooling.
struct OrderView {
    std::string_view cl_ord_id;
    std::string_view symbol;
    std::string_view account;
    Side             side;
    Qty              qty;
    Qty              cum_qty;
    Price            price;
    OrderState       state;
};

struct PositionView {
    Qty position{0};
    Qty open_buy_qty{0};
    Qty open_sell_qty{0};
};

struct EngineStats {
    std::uint64_t checks{0};
    std::uint64_t accepts{0};
    std::uint64_t rejects{0};
    std::array<std::uint64_t, 32> rejects_by_reason{};
};

class RiskEngine {
public:
    explicit RiskEngine(EngineConfig cfg);
    ~RiskEngine();

    RiskEngine(const RiskEngine&) = delete;
    RiskEngine& operator=(const RiskEngine&) = delete;

    // ── Pre-trade checks (call before sending to the venue) ──────────────────
    //
    // On acceptance the engine immediately reserves the order's exposure, so
    // two back-to-back orders cannot both squeeze under the same limit.

    [[nodiscard]] CheckResult check_new(const NewOrderRequest& req, Nanos now);
    [[nodiscard]] CheckResult check_replace(const ReplaceRequest& req, Nanos now);
    [[nodiscard]] CheckResult check_cancel(const CancelRequest& req, Nanos now);

    // ── Venue feedback (call on every execution report) ──────────────────────
    //
    // `cl_ord_id` may be any ClOrdID the engine knows for the order: the
    // original, a pending replace ID, or a pending cancel ID. Unknown IDs
    // return false and change nothing.

    bool on_new_ack(std::string_view cl_ord_id);
    bool on_fill(std::string_view cl_ord_id, Qty last_qty, Price last_px);
    bool on_replaced(std::string_view new_cl_ord_id);
    bool on_replace_rejected(std::string_view new_cl_ord_id);
    bool on_cancelled(std::string_view cl_ord_id);
    bool on_cancel_rejected(std::string_view cancel_cl_ord_id);
    bool on_order_rejected(std::string_view cl_ord_id);  // venue rejected a new order

    // ── Market data ──────────────────────────────────────────────────────────

    // Reference price for the price collar and market-order notional.
    // Returns false for an unknown symbol when unconfigured symbols are off.
    bool set_reference_price(std::string_view symbol, Price px);

    // ── Kill switch ──────────────────────────────────────────────────────────

    void engage_kill_switch() noexcept { killed_ = true; }
    void release_kill_switch() noexcept { killed_ = false; reject_ring_pos_ = 0; reject_ring_fill_ = 0; }
    [[nodiscard]] bool kill_switch_engaged() const noexcept { return killed_; }

    // Visit every live order (for a cancel-all sweep after the kill switch).
    void for_each_open_order(const std::function<void(const OrderView&)>& fn) const;

    // ── Introspection ────────────────────────────────────────────────────────

    [[nodiscard]] PositionView position(std::string_view account, std::string_view symbol) const;
    [[nodiscard]] Notional open_notional(std::string_view account) const;
    [[nodiscard]] std::uint32_t open_orders(std::string_view account) const;
    [[nodiscard]] std::uint32_t live_order_count() const noexcept { return orders_.in_use(); }
    [[nodiscard]] Price reference_price(std::string_view symbol) const;
    [[nodiscard]] std::optional<Qty> cum_qty(std::string_view cl_ord_id) const;
    [[nodiscard]] bool is_pending_replace_id(std::string_view cl_ord_id) const;
    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const EngineConfig& config() const noexcept { return cfg_; }

private:
    struct SymbolState {
        FixedString<kMaxSymbolLen> name;
        SymbolLimits               limits;
        Price                      reference{0};
    };

    struct AccountState {
        FixedString<kMaxAccountLen> name;
        AccountLimits               limits;
        TokenBucket                 bucket;
        bool                        bucket_ready{false};
        std::uint32_t               open_orders{0};
        Notional                    open_notional{0};
    };

    struct PositionState {
        Qty       position{0};
        Qty       open_buy{0};
        Qty       open_sell{0};
        OrderSlot head{kInvalidId};  // intrusive list of live orders
    };

    struct Order {
        ClOrdId    cl_ord_id;
        ClOrdId    pending_replace_id;
        ClOrdId    pending_cancel_id;
        AccountId  account{kInvalidId};
        SymbolId   symbol{kInvalidId};
        std::uint32_t pos{kInvalidId};
        Side       side{Side::Buy};
        OrdType    type{OrdType::Limit};
        OrderState state{OrderState::PendingNew};
        Qty        qty{0};
        Qty        cum{0};
        Price      price{0};
        Qty        pending_qty{0};
        Price      pending_price{0};
        Qty        reserved_qty{0};
        Notional   reserved_notional{0};
        OrderSlot  prev{kInvalidId};
        OrderSlot  next{kInvalidId};
    };

    CheckResult reject(RejectReason r, Nanos now);
    CheckResult accept();

    SymbolId  resolve_symbol(std::string_view sym);
    AccountId resolve_account(std::string_view acc);
    std::uint32_t resolve_position(AccountId a, SymbolId s);

    [[nodiscard]] SymbolId  find_symbol(std::string_view sym) const;
    [[nodiscard]] AccountId find_account(std::string_view acc) const;

    [[nodiscard]] OrderSlot find_order(std::string_view id) const;
    [[nodiscard]] Price effective_price(const Order& o, Price px) const;
    [[nodiscard]] bool would_self_trade(std::uint32_t pos, Side side, OrdType type,
                                        Price px, OrderSlot exclude) const;
    [[nodiscard]] RejectReason check_band(const SymbolState& s, Price px) const;

    void set_reservation(Order& o, Qty qty, Notional notional);
    void recompute_reservation(Order& o);
    void link(Order& o, OrderSlot slot);
    void unlink(Order& o);
    void close_order(OrderSlot slot);

    EngineConfig cfg_;

    std::vector<SymbolState>  symbols_;
    std::vector<AccountState> accounts_;
    std::vector<PositionState> positions_;
    FlatStringMap<kMaxSymbolLen, SymbolId>   symbol_ids_;
    FlatStringMap<kMaxAccountLen, AccountId> account_ids_;
    FlatU64Index                             position_ids_;

    SlotPool<Order>                         orders_;
    FlatStringMap<kMaxClOrdIdLen, OrderSlot> live_ids_;   // any active ClOrdID -> order
    FlatU64Index                             seen_ids_;   // fingerprints of every ClOrdID accepted

    [[nodiscard]] static std::uint64_t fingerprint(std::string_view id) noexcept {
        const std::uint64_t h = hash_bytes(id);
        return h ? h : 1;  // 0 is the index's empty marker
    }
    [[nodiscard]] bool seen(std::string_view id) const noexcept {
        return seen_ids_.find(fingerprint(id)) != kInvalidId;
    }

    bool killed_{false};
    std::unique_ptr<Nanos[]> reject_ring_;
    std::uint32_t reject_ring_pos_{0};
    std::uint32_t reject_ring_fill_{0};

    EngineStats stats_{};
};

} // namespace riskgate
