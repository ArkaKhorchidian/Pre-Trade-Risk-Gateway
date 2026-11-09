#include "riskgate/risk_engine.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace riskgate {

namespace {

// 128-bit intermediate for the band check (ref * bps can exceed int64).
__extension__ using i128 = __int128;

// Price * Qty with overflow detection. On overflow the caller treats the
// order as exceeding any notional limit.
[[nodiscard]] inline bool mul_notional(Price px, Qty qty, Notional& out) noexcept {
    return !__builtin_mul_overflow(px, qty, &out);
}

} // namespace

// ── Construction ──────────────────────────────────────────────────────────────

RiskEngine::RiskEngine(EngineConfig cfg)
    : cfg_(std::move(cfg)),
      symbol_ids_(cfg_.max_symbols),
      account_ids_(cfg_.max_accounts),
      position_ids_(cfg_.max_positions),
      orders_(cfg_.max_orders),
      live_ids_(static_cast<std::size_t>(cfg_.max_orders) * 3),  // id + replace id + cancel id
      seen_ids_(cfg_.max_cl_ord_ids) {
    symbols_.reserve(cfg_.max_symbols);
    accounts_.reserve(cfg_.max_accounts);
    positions_.reserve(cfg_.max_positions);

    if (cfg_.kill_switch_reject_threshold > 0)
        reject_ring_ = std::make_unique<Nanos[]>(cfg_.kill_switch_reject_threshold);

    // Pre-register configured symbols and accounts so the first order for
    // them does not pay for interning.
    for (const auto& [name, limits] : cfg_.symbols) {
        if (resolve_symbol(name) == kInvalidId)
            throw std::runtime_error("cannot register symbol: " + name);
    }
    for (const auto& [name, limits] : cfg_.accounts) {
        if (resolve_account(name) == kInvalidId)
            throw std::runtime_error("cannot register account: " + name);
    }
}

RiskEngine::~RiskEngine() = default;

// ── Lookup / interning ────────────────────────────────────────────────────────

SymbolId RiskEngine::find_symbol(std::string_view sym) const {
    const SymbolId* id = symbol_ids_.find(sym);
    return id ? *id : kInvalidId;
}

AccountId RiskEngine::find_account(std::string_view acc) const {
    const AccountId* id = account_ids_.find(acc);
    return id ? *id : kInvalidId;
}

SymbolId RiskEngine::resolve_symbol(std::string_view sym) {
    if (const SymbolId* id = symbol_ids_.find(sym)) return *id;
    if (sym.empty() || sym.size() > kMaxSymbolLen) return kInvalidId;
    if (symbols_.size() >= cfg_.max_symbols) return kInvalidId;

    const SymbolLimits* limits = nullptr;
    // Heterogeneous lookup (std::less<>): no temporary std::string.
    if (auto it = cfg_.symbols.find(sym); it != cfg_.symbols.end()) {
        limits = &it->second;
    } else if (cfg_.allow_unconfigured_symbols) {
        limits = &cfg_.symbol_defaults;
    } else {
        return kInvalidId;
    }

    const auto id = static_cast<SymbolId>(symbols_.size());
    if (!symbol_ids_.insert(sym, id)) return kInvalidId;
    SymbolState st;
    st.name      = *FixedString<kMaxSymbolLen>::from(sym);
    st.limits    = *limits;
    st.reference = limits->reference_price;
    symbols_.push_back(st);  // capacity reserved: no reallocation
    return id;
}

AccountId RiskEngine::resolve_account(std::string_view acc) {
    if (const AccountId* id = account_ids_.find(acc)) return *id;
    if (acc.size() > kMaxAccountLen) return kInvalidId;
    if (accounts_.size() >= cfg_.max_accounts) return kInvalidId;

    const AccountLimits* limits = nullptr;
    if (auto it = cfg_.accounts.find(acc); it != cfg_.accounts.end()) {
        limits = &it->second;
    } else if (cfg_.allow_unconfigured_accounts) {
        limits = &cfg_.account_defaults;
    } else {
        return kInvalidId;
    }

    const auto id = static_cast<AccountId>(accounts_.size());
    if (!account_ids_.insert(acc, id)) return kInvalidId;
    AccountState st;
    st.name   = *FixedString<kMaxAccountLen>::from(acc);
    st.limits = *limits;
    accounts_.push_back(st);
    return id;
}

std::uint32_t RiskEngine::resolve_position(AccountId a, SymbolId s) {
    // +1 so that (0, 0) does not produce the reserved key 0.
    const std::uint64_t key = (static_cast<std::uint64_t>(a) << 32 | s) + 1;
    std::uint32_t idx = position_ids_.find(key);
    if (idx != kInvalidId) return idx;
    if (positions_.size() >= cfg_.max_positions) return kInvalidId;
    idx = static_cast<std::uint32_t>(positions_.size());
    if (!position_ids_.insert(key, idx)) return kInvalidId;
    positions_.push_back(PositionState{});
    return idx;
}

OrderSlot RiskEngine::find_order(std::string_view id) const {
    const OrderSlot* s = live_ids_.find(id);
    return s ? *s : kInvalidId;
}

// ── Result bookkeeping ────────────────────────────────────────────────────────

CheckResult RiskEngine::accept() {
    ++stats_.checks;
    ++stats_.accepts;
    return CheckResult::ok();
}

CheckResult RiskEngine::reject(RejectReason r, Nanos now) {
    ++stats_.checks;
    ++stats_.rejects;
    ++stats_.rejects_by_reason[static_cast<std::size_t>(r)];

    // Auto kill switch: a ring of the last N reject timestamps. If the oldest
    // of the last N is inside the window, N rejects happened within it. A
    // strategy stuck in a reject loop is the classic runaway-algo signature.
    const std::uint32_t n = cfg_.kill_switch_reject_threshold;
    if (n > 0 && r != RejectReason::KillSwitch) {
        reject_ring_[reject_ring_pos_] = now;
        reject_ring_pos_ = (reject_ring_pos_ + 1) % n;
        if (reject_ring_fill_ < n) ++reject_ring_fill_;
        if (reject_ring_fill_ == n) {
            const Nanos oldest = reject_ring_[reject_ring_pos_];  // next slot to overwrite
            if (now - oldest <= cfg_.kill_switch_window_ns) killed_ = true;
        }
    }
    return CheckResult::reject(r);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

RejectReason RiskEngine::check_band(const SymbolState& s, Price px) const {
    const std::uint32_t band = s.limits.price_band_bps;
    if (band == 0 || s.reference <= 0) return RejectReason::None;
    // |px - ref| / ref > band / 10_000   <=>   |px - ref| * 10_000 > ref * band
    const i128 diff = static_cast<i128>(px > s.reference ? px - s.reference : s.reference - px);
    if (diff * 10'000 > static_cast<i128>(s.reference) * band) return RejectReason::PriceOutsideBand;
    return RejectReason::None;
}

bool RiskEngine::would_self_trade(std::uint32_t pos, Side side, OrdType type,
                                  Price px, OrderSlot exclude) const {
    // Positions are per (account, symbol), so every order on this list belongs
    // to the same account. Lists are short (a handful of resting orders per
    // symbol), so a scan beats maintaining a sorted structure.
    for (OrderSlot s = positions_[pos].head; s != kInvalidId; s = orders_[s].next) {
        if (s == exclude) continue;
        const Order& o = orders_[s];
        if (o.side == side) continue;
        if (type == OrdType::Market || o.type == OrdType::Market) return true;
        if (side == Side::Buy  && px >= o.price) return true;
        if (side == Side::Sell && px <= o.price) return true;
    }
    return false;
}

void RiskEngine::set_reservation(Order& o, Qty qty, Notional notional) {
    PositionState& p = positions_[o.pos];
    AccountState&  a = accounts_[o.account];
    const Qty dq = qty - o.reserved_qty;
    if (o.side == Side::Buy) p.open_buy += dq; else p.open_sell += dq;
    a.open_notional += notional - o.reserved_notional;
    o.reserved_qty      = qty;
    o.reserved_notional = notional;
}

Price RiskEngine::effective_price(const Order& o, Price px) const {
    return o.type == OrdType::Market ? symbols_[o.symbol].reference : px;
}

void RiskEngine::recompute_reservation(Order& o) {
    Qty leaves = std::max<Qty>(o.qty - o.cum, 0);
    Notional n = 0;
    if (!mul_notional(effective_price(o, o.price), leaves, n)) n = INT64_MAX;
    if (o.state == OrderState::PendingReplace) {
        const Qty pl = std::max<Qty>(o.pending_qty - o.cum, 0);
        Notional pn = 0;
        if (!mul_notional(o.pending_price, pl, pn)) pn = INT64_MAX;
        leaves = std::max(leaves, pl);
        n = std::max(n, pn);
    }
    set_reservation(o, leaves, n);
}

void RiskEngine::link(Order& o, OrderSlot slot) {
    PositionState& p = positions_[o.pos];
    o.prev = kInvalidId;
    o.next = p.head;
    if (p.head != kInvalidId) orders_[p.head].prev = slot;
    p.head = slot;
}

void RiskEngine::unlink(Order& o) {
    PositionState& p = positions_[o.pos];
    if (o.prev != kInvalidId) orders_[o.prev].next = o.next; else p.head = o.next;
    if (o.next != kInvalidId) orders_[o.next].prev = o.prev;
    o.prev = o.next = kInvalidId;
}

void RiskEngine::close_order(OrderSlot slot) {
    Order& o = orders_[slot];
    set_reservation(o, 0, 0);
    unlink(o);
    --accounts_[o.account].open_orders;
    live_ids_.erase(o.cl_ord_id.view());
    if (!o.pending_replace_id.empty()) live_ids_.erase(o.pending_replace_id.view());
    if (!o.pending_cancel_id.empty())  live_ids_.erase(o.pending_cancel_id.view());
    orders_.release(slot);
}

// ── check_new ─────────────────────────────────────────────────────────────────

CheckResult RiskEngine::check_new(const NewOrderRequest& r, Nanos now) {
    if (killed_) return reject(RejectReason::KillSwitch, now);

    // Static validation: cheapest first.
    if (r.qty <= 0 || r.cl_ord_id.empty() || r.symbol.empty() ||
        (r.type == OrdType::Limit && r.price <= 0))
        return reject(RejectReason::InvalidOrder, now);
    if (r.cl_ord_id.size() > kMaxClOrdIdLen) return reject(RejectReason::ClOrdIdTooLong, now);
    if (seen(r.cl_ord_id)) return reject(RejectReason::DuplicateClOrdId, now);

    // Reference data.
    const SymbolId sid = resolve_symbol(r.symbol);
    if (sid == kInvalidId) return reject(RejectReason::UnknownSymbol, now);
    const AccountId aid = resolve_account(r.account);
    if (aid == kInvalidId) return reject(RejectReason::UnknownAccount, now);

    SymbolState&  sym = symbols_[sid];
    AccountState& acc = accounts_[aid];
    const SymbolLimits& sl = sym.limits;

    if (sl.restricted) return reject(RejectReason::RestrictedSymbol, now);
    if (r.type == OrdType::Market && !cfg_.allow_market_orders)
        return reject(RejectReason::MarketOrdersDisabled, now);

    // Price collar.
    if (sym.reference <= 0 && (sl.require_reference_price || r.type == OrdType::Market))
        return reject(RejectReason::NoReferencePrice, now);
    if (r.type == OrdType::Limit) {
        if (auto b = check_band(sym, r.price); b != RejectReason::None) return reject(b, now);
    }

    // Fat finger: size and notional of this order alone.
    const Price px = r.type == OrdType::Market ? sym.reference : r.price;
    if (sl.max_order_qty > 0 && r.qty > sl.max_order_qty) return reject(RejectReason::MaxOrderQty, now);
    Notional notional = 0;
    const bool fits = mul_notional(px, r.qty, notional);
    if (sl.max_order_notional > 0 && (!fits || notional > sl.max_order_notional))
        return reject(RejectReason::MaxOrderNotional, now);
    if (!fits) return reject(RejectReason::MaxOpenNotional, now);

    // Worst-case position: current position plus every open order on the
    // same side filling in full, plus this one.
    const std::uint32_t pos = resolve_position(aid, sid);
    if (pos == kInvalidId) return reject(RejectReason::CapacityExceeded, now);
    const PositionState& p = positions_[pos];
    if (sl.max_position > 0) {
        const Qty worst = r.side == Side::Buy ? p.position + p.open_buy + r.qty
                                              : -p.position + p.open_sell + r.qty;
        if (worst > sl.max_position) return reject(RejectReason::MaxPosition, now);
    }

    // Account-level exposure.
    if (acc.limits.max_open_notional > 0 &&
        acc.open_notional > acc.limits.max_open_notional - notional)
        return reject(RejectReason::MaxOpenNotional, now);
    if (acc.limits.max_open_orders > 0 && acc.open_orders >= acc.limits.max_open_orders)
        return reject(RejectReason::MaxOpenOrders, now);

    if (cfg_.self_trade_prevention && would_self_trade(pos, r.side, r.type, r.price, kInvalidId))
        return reject(RejectReason::SelfTrade, now);

    // Throttle last: only orders that will actually reach the venue spend a token.
    if (!acc.bucket_ready) {
        acc.bucket = TokenBucket(acc.limits.orders_per_second, acc.limits.burst, now);
        acc.bucket_ready = true;
    }
    if (!acc.bucket.available(now)) return reject(RejectReason::RateLimited, now);

    // Capacity: fail closed.
    if (seen_ids_.size() >= cfg_.max_cl_ord_ids) return reject(RejectReason::CapacityExceeded, now);
    const OrderSlot slot = orders_.acquire();
    if (slot == kInvalidId) return reject(RejectReason::CapacityExceeded, now);
    if (!live_ids_.insert(r.cl_ord_id, slot)) {
        orders_.release(slot);
        return reject(RejectReason::CapacityExceeded, now);
    }
    seen_ids_.insert(fingerprint(r.cl_ord_id), 1);

    // Commit.
    acc.bucket.consume();
    Order& o   = orders_[slot];
    o.cl_ord_id = *ClOrdId::from(r.cl_ord_id);
    o.account   = aid;
    o.symbol    = sid;
    o.pos       = pos;
    o.side      = r.side;
    o.type      = r.type;
    o.state     = OrderState::PendingNew;
    o.qty       = r.qty;
    o.price     = px;
    link(o, slot);
    ++acc.open_orders;
    set_reservation(o, r.qty, notional);
    return accept();
}

// ── check_replace ─────────────────────────────────────────────────────────────

CheckResult RiskEngine::check_replace(const ReplaceRequest& r, Nanos now) {
    // A replace can increase risk, so the kill switch blocks it. Cancel instead.
    if (killed_) return reject(RejectReason::KillSwitch, now);
    if (r.qty <= 0 || r.price <= 0 || r.cl_ord_id.empty())
        return reject(RejectReason::InvalidOrder, now);
    if (r.cl_ord_id.size() > kMaxClOrdIdLen) return reject(RejectReason::ClOrdIdTooLong, now);
    if (seen(r.cl_ord_id)) return reject(RejectReason::DuplicateClOrdId, now);

    const OrderSlot slot = find_order(r.orig_cl_ord_id);
    if (slot == kInvalidId) return reject(RejectReason::UnknownOrder, now);
    Order& o = orders_[slot];
    if (o.state != OrderState::Open || !o.pending_cancel_id.empty() ||
        o.cl_ord_id.view() != r.orig_cl_ord_id)
        return reject(RejectReason::OrderNotOpen, now);
    if (o.type == OrdType::Market) return reject(RejectReason::InvalidOrder, now);

    const Qty new_leaves = r.qty - o.cum;
    if (new_leaves <= 0) return reject(RejectReason::InvalidOrder, now);

    const SymbolState& sym = symbols_[o.symbol];
    const SymbolLimits& sl = sym.limits;
    AccountState& acc = accounts_[o.account];

    if (auto b = check_band(sym, r.price); b != RejectReason::None) return reject(b, now);
    if (sl.max_order_qty > 0 && r.qty > sl.max_order_qty) return reject(RejectReason::MaxOrderQty, now);

    Notional order_notional = 0, leaves_notional = 0;
    const bool fits = mul_notional(r.price, r.qty, order_notional) &&
                      mul_notional(r.price, new_leaves, leaves_notional);
    if (sl.max_order_notional > 0 && (!fits || order_notional > sl.max_order_notional))
        return reject(RejectReason::MaxOrderNotional, now);
    if (!fits) return reject(RejectReason::MaxOpenNotional, now);

    // Until the venue confirms, either the old or the new order may be live,
    // so reserve the larger of the two.
    const Qty      res_qty = std::max(o.reserved_qty, new_leaves);
    const Notional res_not = std::max(o.reserved_notional, leaves_notional);

    const PositionState& p = positions_[o.pos];
    if (sl.max_position > 0) {
        const Qty open_same = (o.side == Side::Buy ? p.open_buy : p.open_sell) - o.reserved_qty + res_qty;
        const Qty worst = o.side == Side::Buy ? p.position + open_same : -p.position + open_same;
        if (worst > sl.max_position) return reject(RejectReason::MaxPosition, now);
    }
    if (acc.limits.max_open_notional > 0 &&
        acc.open_notional - o.reserved_notional > acc.limits.max_open_notional - res_not)
        return reject(RejectReason::MaxOpenNotional, now);

    if (cfg_.self_trade_prevention && would_self_trade(o.pos, o.side, OrdType::Limit, r.price, slot))
        return reject(RejectReason::SelfTrade, now);

    if (!acc.bucket.available(now)) return reject(RejectReason::RateLimited, now);
    if (seen_ids_.size() >= cfg_.max_cl_ord_ids) return reject(RejectReason::CapacityExceeded, now);
    if (!live_ids_.insert(r.cl_ord_id, slot)) return reject(RejectReason::CapacityExceeded, now);
    seen_ids_.insert(fingerprint(r.cl_ord_id), 1);

    acc.bucket.consume();
    o.pending_replace_id = *ClOrdId::from(r.cl_ord_id);
    o.pending_qty   = r.qty;
    o.pending_price = r.price;
    o.state         = OrderState::PendingReplace;
    set_reservation(o, res_qty, res_not);
    return accept();
}

// ── check_cancel ──────────────────────────────────────────────────────────────

CheckResult RiskEngine::check_cancel(const CancelRequest& r, Nanos now) {
    // Deliberately no kill-switch or throttle check here.
    if (r.cl_ord_id.empty()) return reject(RejectReason::InvalidOrder, now);
    if (r.cl_ord_id.size() > kMaxClOrdIdLen) return reject(RejectReason::ClOrdIdTooLong, now);
    if (seen(r.cl_ord_id)) return reject(RejectReason::DuplicateClOrdId, now);

    const OrderSlot slot = find_order(r.orig_cl_ord_id);
    if (slot == kInvalidId) return reject(RejectReason::UnknownOrder, now);
    Order& o = orders_[slot];
    if (!o.pending_cancel_id.empty()) return reject(RejectReason::OrderNotOpen, now);

    if (!live_ids_.insert(r.cl_ord_id, slot)) return reject(RejectReason::CapacityExceeded, now);
    seen_ids_.insert(fingerprint(r.cl_ord_id), 1);  // best effort: never block a cancel on this
    o.pending_cancel_id = *ClOrdId::from(r.cl_ord_id);
    return accept();
}

// ── Venue feedback ────────────────────────────────────────────────────────────

bool RiskEngine::on_new_ack(std::string_view id) {
    const OrderSlot slot = find_order(id);
    if (slot == kInvalidId) return false;
    Order& o = orders_[slot];
    if (o.state == OrderState::PendingNew) o.state = OrderState::Open;
    return true;
}

bool RiskEngine::on_fill(std::string_view id, Qty last_qty, Price last_px) {
    const OrderSlot slot = find_order(id);
    if (slot == kInvalidId || last_qty <= 0) return false;
    Order& o = orders_[slot];
    if (o.state == OrderState::PendingNew) o.state = OrderState::Open;

    PositionState& p = positions_[o.pos];
    p.position += o.side == Side::Buy ? last_qty : -last_qty;
    o.cum += last_qty;
    if (cfg_.fills_update_reference && last_px > 0) symbols_[o.symbol].reference = last_px;

    // Fully filled against the live quantity (and any pending replace target).
    const Qty target = o.state == OrderState::PendingReplace ? std::max(o.qty, o.pending_qty) : o.qty;
    if (o.cum >= target) {
        close_order(slot);
    } else {
        recompute_reservation(o);
    }
    return true;
}

bool RiskEngine::on_replaced(std::string_view new_id) {
    const OrderSlot slot = find_order(new_id);
    if (slot == kInvalidId) return false;
    Order& o = orders_[slot];
    if (o.state != OrderState::PendingReplace || o.pending_replace_id.view() != new_id) return false;
    live_ids_.erase(o.cl_ord_id.view());
    o.cl_ord_id = o.pending_replace_id;
    o.pending_replace_id = ClOrdId{};
    o.qty   = o.pending_qty;
    o.price = o.pending_price;
    o.state = OrderState::Open;
    if (o.cum >= o.qty) {
        close_order(slot);
    } else {
        recompute_reservation(o);
    }
    return true;
}

bool RiskEngine::on_replace_rejected(std::string_view new_id) {
    const OrderSlot slot = find_order(new_id);
    if (slot == kInvalidId) return false;
    Order& o = orders_[slot];
    if (o.state != OrderState::PendingReplace || o.pending_replace_id.view() != new_id) return false;
    live_ids_.erase(new_id);
    o.pending_replace_id = ClOrdId{};
    o.state = OrderState::Open;
    recompute_reservation(o);
    return true;
}

bool RiskEngine::on_cancelled(std::string_view id) {
    const OrderSlot slot = find_order(id);
    if (slot == kInvalidId) return false;
    close_order(slot);
    return true;
}

bool RiskEngine::on_cancel_rejected(std::string_view cancel_id) {
    const OrderSlot slot = find_order(cancel_id);
    if (slot == kInvalidId) return false;
    Order& o = orders_[slot];
    if (o.pending_cancel_id.view() != cancel_id) return false;
    live_ids_.erase(cancel_id);
    o.pending_cancel_id = ClOrdId{};
    return true;
}

bool RiskEngine::on_order_rejected(std::string_view id) {
    return on_cancelled(id);
}

// ── Market data / introspection ───────────────────────────────────────────────

bool RiskEngine::set_reference_price(std::string_view symbol, Price px) {
    const SymbolId sid = resolve_symbol(symbol);
    if (sid == kInvalidId) return false;
    symbols_[sid].reference = px;
    return true;
}

std::optional<Qty> RiskEngine::cum_qty(std::string_view cl_ord_id) const {
    const OrderSlot slot = find_order(cl_ord_id);
    if (slot == kInvalidId) return std::nullopt;
    return orders_[slot].cum;
}

bool RiskEngine::is_pending_replace_id(std::string_view cl_ord_id) const {
    const OrderSlot slot = find_order(cl_ord_id);
    return slot != kInvalidId && orders_[slot].pending_replace_id.view() == cl_ord_id;
}

Price RiskEngine::reference_price(std::string_view symbol) const {
    const SymbolId sid = find_symbol(symbol);
    return sid == kInvalidId ? 0 : symbols_[sid].reference;
}

PositionView RiskEngine::position(std::string_view account, std::string_view symbol) const {
    const AccountId a = find_account(account);
    const SymbolId  s = find_symbol(symbol);
    if (a == kInvalidId || s == kInvalidId) return {};
    const std::uint32_t idx = position_ids_.find((static_cast<std::uint64_t>(a) << 32 | s) + 1);
    if (idx == kInvalidId) return {};
    const PositionState& p = positions_[idx];
    return {p.position, p.open_buy, p.open_sell};
}

Notional RiskEngine::open_notional(std::string_view account) const {
    const AccountId a = find_account(account);
    return a == kInvalidId ? 0 : accounts_[a].open_notional;
}

std::uint32_t RiskEngine::open_orders(std::string_view account) const {
    const AccountId a = find_account(account);
    return a == kInvalidId ? 0 : accounts_[a].open_orders;
}

void RiskEngine::for_each_open_order(const std::function<void(const OrderView&)>& fn) const {
    for (const PositionState& p : positions_) {
        for (OrderSlot s = p.head; s != kInvalidId; s = orders_[s].next) {
            const Order& o = orders_[s];
            fn(OrderView{o.cl_ord_id.view(), symbols_[o.symbol].name.view(),
                         accounts_[o.account].name.view(), o.side, o.qty, o.cum, o.price, o.state});
        }
    }
}

} // namespace riskgate
