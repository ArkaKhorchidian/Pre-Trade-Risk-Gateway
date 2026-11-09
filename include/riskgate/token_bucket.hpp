#pragma once

// Integer token bucket for order-rate throttling.
//
// Tokens are tracked in "nano-tokens" (1 token = 1e9 units) so refill is exact
// integer arithmetic: elapsed_ns * rate_per_sec nano-tokens accrue per tick.
// No floating point, no drift, deterministic in tests.

#include "types.hpp"

#include <algorithm>
#include <cstdint>

namespace riskgate {

class TokenBucket {
public:
    static constexpr std::uint64_t kUnit = 1'000'000'000ULL;

    TokenBucket() = default;

    TokenBucket(std::uint32_t rate_per_sec, std::uint32_t burst, Nanos now) noexcept
        : rate_(rate_per_sec),
          cap_(static_cast<std::uint64_t>(std::max<std::uint32_t>(burst, 1)) * kUnit),
          level_(cap_),
          last_(now) {}

    // Returns true if one token is available at time `now` (after refill).
    // Does not consume: the engine only spends a token once every other check
    // has passed, because a rejected order never reaches the venue and should
    // not count against the venue's message-rate limit.
    [[nodiscard]] bool available(Nanos now) noexcept {
        if (rate_ == 0) return true;  // unlimited
        refill(now);
        return level_ >= kUnit;
    }

    void consume() noexcept {
        if (rate_ == 0) return;
        level_ -= kUnit;
    }

    [[nodiscard]] std::uint64_t tokens(Nanos now) noexcept {
        refill(now);
        return level_ / kUnit;
    }

private:
    void refill(Nanos now) noexcept {
        if (now <= last_) return;
        const std::uint64_t elapsed = now - last_;
        last_ = now;
        // elapsed (ns) * rate (tokens/s) = nano-tokens. Guard the multiply:
        // after a long idle period the bucket is simply full.
        if (elapsed >= cap_ / std::max<std::uint64_t>(rate_, 1)) {
            level_ = cap_;
            return;
        }
        level_ = std::min(cap_, level_ + elapsed * rate_);
    }

    std::uint32_t rate_{0};
    std::uint64_t cap_{0};
    std::uint64_t level_{0};
    Nanos         last_{0};
};

} // namespace riskgate
