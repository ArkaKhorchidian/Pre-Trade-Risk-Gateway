# Pre-Trade Risk Gateway

A C++20 pre-trade risk engine with a FIX 4.2 adapter. It sits between the
strategy / OMS and the venue session and checks every order before it reaches
the wire: price collars, fat-finger size and notional limits, worst-case
position limits, account exposure, order-rate throttles, self-trade
prevention, a restricted list, duplicate ClOrdID detection and a kill switch.

It is designed as the risk layer for
[FIX-Protocol-Parser-Order-Router](https://github.com/ArkaKhorchidian/FIX-Protocol-Parser-Order-Router):
the router decides *where* an order goes, this gateway decides *whether* it
goes at all.

```
 strategy / OMS
       │  NewOrderSingle (D) / CancelReplace (G) / Cancel (F)
       ▼
 ┌──────────────────┐   reject ──► ExecutionReport 35=8 (150=8, 103, 58=RISK:...)
 │  FixGateway      │          or  OrderCancelReject 35=9 (434, 102)
 │  └─ RiskEngine   │
 └────────┬─────────┘
          │ forward
          ▼
 ┌──────────────────┐        ┌───────┐
 │   OrderRouter    │──FIX──►│ venue │
 └──────────────────┘        └───┬───┘
          ▲                      │ ExecutionReport (8) / CancelReject (9)
          └────── FixGateway::on_inbound() updates positions ◄──┘
```

## Why this exists

Any broker-dealer with market access in the US must run pre-trade controls
under [SEC Rule 15c3-5](https://www.federalregister.gov/documents/2010/11/15/2010-28303/risk-management-controls-for-brokers-or-dealers-with-market-access)
(the Market Access Rule): reject orders that exceed credit or capital
thresholds, reject erroneous orders that break price or size parameters, and
block orders that are not allowed (such as restricted securities). Firms
trading through a broker's direct market access face the same checks from the
broker and usually run their own in front of them, tighter, so a bad order
dies in their own process instead of at the broker.

The hard part is not the arithmetic. It is getting the state right (open
orders, partial fills, in-flight replaces) and doing it in well under a
microsecond so the check never becomes the reason an order is late.

## Checks

Evaluated in this order; the first failure wins. Cheap static checks run
before anything that touches state.

| # | Check | Reject reason | Scope |
|---|-------|---------------|-------|
| 1 | Kill switch engaged | `KillSwitch` | engine |
| 2 | Quantity, price, side, order type valid | `InvalidOrder` | order |
| 3 | ClOrdID length / never used before this session | `ClOrdIdTooLong`, `DuplicateClOrdId` | engine |
| 4 | Symbol and account known (or defaults allowed) | `UnknownSymbol`, `UnknownAccount` | config |
| 5 | Symbol not on the restricted list | `RestrictedSymbol` | symbol |
| 6 | Market orders allowed | `MarketOrdersDisabled` | engine |
| 7 | Reference price present | `NoReferencePrice` | symbol |
| 8 | Limit price within ±N bps of reference | `PriceOutsideBand` | symbol |
| 9 | Order quantity ≤ max | `MaxOrderQty` | symbol |
| 10 | Order notional ≤ max | `MaxOrderNotional` | symbol |
| 11 | Worst-case position ≤ max (position + all open same-side orders + this) | `MaxPosition` | account × symbol |
| 12 | Open notional ≤ max | `MaxOpenNotional` | account |
| 13 | Open order count ≤ max | `MaxOpenOrders` | account |
| 14 | Would not cross a resting order from the same account | `SelfTrade` | account × symbol |
| 15 | Token bucket has a token | `RateLimited` | account |
| 16 | Table capacity available | `CapacityExceeded` | engine |

Replaces go through the same checks against the new terms. Cancels skip the
kill switch and the throttle (see below).

## Design decisions

**Cancels are never blocked.** Not by the kill switch, not by the rate
limit. A risk system that stops you from pulling orders in a runaway
situation has become the risk. After the kill switch fires,
`for_each_open_order()` gives you the sweep list.

**Exposure is worst case and reserved at check time.** An accepted order
immediately counts against position and notional limits as if it will fully
fill, so two orders sent back-to-back can't both squeeze under the same limit
before either is acknowledged. Fills move exposure from "open" to "position";
cancels and venue rejects release it.

**In-flight replaces reserve max(old, new).** Between sending a
CancelReplace and receiving the venue's answer, either the old or the new
order may be live (the replace can be rejected, or the old order can fill
before the replace lands). The engine reserves the larger of the two until
the venue confirms, then settles to the new terms, or restores the old ones on
reject.

**Throttle tokens are spent last.** A rejected order never reaches the venue
and shouldn't count against the venue's message-rate limit, so the token
bucket is checked late and only consumed once every other check has passed.
The bucket uses integer nano-tokens: no floating point drift, deterministic
in tests.

**No allocation, no floating point, no locks on the check path.** All tables
are fixed capacity, allocated at construction:
open-addressing hash maps with backward-shift deletion (no tombstones, so
lookups don't degrade over a full session of order churn), a slot pool for
orders, and intrusive per-position order lists for the self-trade scan.
Prices are integers in 1/10,000 units (NASDAQ's convention), parsed from FIX
decimals exactly without going through `double`. The engine is
single-threaded: run one per gateway thread and shard by account.

**Fail closed.** If a table is full, the order is rejected
(`CapacityExceeded`), never let through. Unsupported FIX order types (stops,
pegs) are rejected rather than waved through unchecked. Notional overflow
counts as a limit breach.

**Duplicate detection uses 64-bit fingerprints.** Remembering every ClOrdID
of a session as full strings costs ~40 bytes each and turns every lookup into
a cache miss. Fingerprints cost 12 bytes. A collision (≈ n²/2⁶⁵, about 3×10⁻⁸
for a million IDs) rejects a legitimate order. It can never let a duplicate
through.

**Automatic kill switch.** If N risk rejects happen within a time window,
the engine trips. A strategy stuck in a reject loop is the classic signature
of a runaway algo. Implemented as a ring of the last N reject timestamps:
O(1), no allocation.

## Performance

`riskgate_bench`: 500 symbols, 8 accounts, every limit enabled, ~2,000 live
orders held at steady state by acking and then filling or cancelling older
orders. Each call timed individually; clock overhead (22 ns) subtracted.

| Path | p50 | p90 | p99 | p99.9 |
|------|-----|-----|-----|-------|
| `RiskEngine::check_new` | 465 ns | 655 ns | 840 ns | 2.3 µs |
| `FixGateway::on_outbound` (FIX parse + check) | 700 ns | 900 ns | 1.1 µs | 14 µs |

Measured on a shared 2 vCPU cloud VM (Intel Xeon @ 2.30 GHz), no core
isolation or CPU pinning, so the tail reflects the VM's noisy neighbours
more than the code. A check executes about 450 instructions (callgrind). Most
of the remaining time is cache misses on the order and ClOrdID tables at
realistic sizes. On an isolated core, expect the p50 to drop considerably.

## Build and run

Requires CMake ≥ 3.20 and a C++20 compiler (GCC 12+, Clang 16+). GoogleTest
is fetched automatically. No other dependencies.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
(cd build && ctest --output-on-failure)     # 54 tests, ASan + UBSan
./build/riskgate_bench
```

### Replay a FIX session

`riskgate_replay` runs a FIX log (SOH or `|` delimited) through the gateway:
orders are checked, execution reports update state.

```
$ ./build/riskgate_replay --config config/example.ini examples/demo_session.log
PASS   35=D ClOrdID=ORD1
REJECT 35=D ClOrdID=ORD2               PriceOutsideBand
REJECT 35=D ClOrdID=ORD3               RestrictedSymbol
REJECT 35=D ClOrdID=ORD4               MaxOrderQty
REJECT 35=D ClOrdID=ORD5               SelfTrade
REJECT 35=D ClOrdID=ORD1               DuplicateClOrdId
REJECT 35=D ClOrdID=ORD6               MarketOrdersDisabled

── Summary ─────────────────────────────────────────
order messages: 1 forwarded, 6 rejected; 2 execution reports applied
...
Positions (account / symbol: position, open buy, open sell)
  DESK1      AAPL          100        0        0
```

It reads the logs from FIX-Protocol-Parser-Order-Router directly:

```bash
./build/riskgate_replay ../FIX-Protocol-Parser-Order-Router/data/sample_session.txt
```

## Using it

### Engine API

```cpp
#include "riskgate/risk_engine.hpp"
using namespace riskgate;

RiskEngine risk(EngineConfig::from_file("config/example.ini"));

NewOrderRequest req{"ORD1", "DESK1", "AAPL", Side::Buy, OrdType::Limit, 100, to_price(150.00)};
if (CheckResult r = risk.check_new(req, now_ns); !r) {
    log_reject(reject_reason_name(r.reason));
    return;
}
send_to_venue(...);

// From the venue session:
risk.on_new_ack("ORD1");
risk.on_fill("ORD1", 40, to_price(150.01));
risk.on_cancelled("ORD1");
```

### Plugging into FIX-Protocol-Parser-Order-Router

The router generates the ClOrdID inside `send_new_order`, so the check goes
right after it and before the order is registered and sent:

```cpp
bool OrderRouter::send_new_order(const NewOrder& order) {
    Venue* venue = venue_for_symbol(order.symbol);
    if (!venue) return false;
    std::string cl_ord_id = next_cl_ord_id();

    riskgate::NewOrderRequest req{
        cl_ord_id, order.account, order.symbol,
        order.side == "1" ? riskgate::Side::Buy : riskgate::Side::Sell,
        order.ord_type == "1" ? riskgate::OrdType::Market : riskgate::OrdType::Limit,
        static_cast<riskgate::Qty>(order.order_qty), riskgate::to_price(order.price)};

    if (auto r = risk_.check_new(req, now_ns()); !r) {
        spdlog::warn("[Risk] {} rejected: {}", cl_ord_id, riskgate::reject_reason_name(r.reason));
        return false;
    }
    // ... existing PendingOrder registration and do_send_new_order()
}

// In the venue message callback, before or alongside on_execution_report():
risk_gateway_.on_inbound(msg.raw());
```

### As a FIX filter

`FixGateway` works on raw FIX messages, for a gateway process that sits on
the wire between an OMS and a venue session:

```cpp
riskgate::fix::FixGateway gw(risk);

auto d = gw.on_outbound(client_msg, now_ns());   // 35=D / F / G
if (d.forward) venue_session.send(client_msg);
else           client_session.send_app(d.reject_fields);  // 35=8 or 35=9, session layer adds header

gw.on_inbound(venue_msg);                         // 35=8 / 9
```

Rejects map onto standard FIX 4.2 codes: `OrdRejReason` (103) 1 = unknown
symbol, 3 = order exceeds limit, 6 = duplicate order; `CxlRejReason` (102)
1 = unknown order, 3 = already pending. The engine's own reason goes in
`Text` (58) as `RISK:<Reason>`.

## Configuration

See [`config/example.ini`](config/example.ini). Per-symbol and per-account
sections inherit from `[symbol_defaults]` / `[account_defaults]`, whatever
order they appear in. Unknown keys are errors, reported with a line number,
because a typo in a risk config should never silently disable a limit.

| Section | Key | Meaning |
|---------|-----|---------|
| `engine` | `max_orders`, `max_cl_ord_ids`, `max_symbols`, `max_accounts`, `max_positions` | table capacities |
| | `allow_unconfigured_symbols` / `_accounts` | fall back to defaults, or reject |
| | `allow_market_orders`, `self_trade_prevention`, `fills_update_reference` | switches |
| | `kill_switch_reject_threshold`, `kill_switch_window_ms` | auto kill switch |
| `symbol_*` | `max_order_qty`, `max_order_notional`, `max_position` | fat finger and position |
| | `price_band_bps`, `reference_price`, `require_reference_price` | price collar |
| | `restricted` | restricted list |
| `account_*` | `max_open_orders`, `max_open_notional` | exposure |
| | `orders_per_second`, `burst` | throttle |

## Layout

```
include/riskgate/
  types.hpp          fixed-point prices, requests, reject reasons
  containers.hpp     fixed-capacity hash maps, slot pool
  token_bucket.hpp   integer token bucket
  config.hpp         limits + INI loader
  risk_engine.hpp    the engine
  fix_gateway.hpp    FIX 4.2 adapter, reject builders, framing
src/                 engine and adapter implementation
tools/riskgate_replay.cpp
bench/bench_risk.cpp
tests/               54 GoogleTest cases incl. a 20k-step randomised
                     invariant test (aggregates == sum over live orders)
```

## Not yet

- Start-of-day position load and drop-copy reconciliation
- Per-venue limits and per-strategy sub-accounts
- Options and multi-leg instruments (delta-adjusted exposure)
- Shared-memory control plane for live limit changes and kill switch from an ops console

## License

MIT
