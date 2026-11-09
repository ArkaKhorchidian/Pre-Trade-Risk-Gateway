#pragma once

// Allocation-free building blocks for the check path.
//
// Everything here allocates once, at construction, and never again. A risk
// check that calls malloc can stall behind another thread holding the
// allocator lock, which turns a 100 ns check into a 50 us one at the exact
// moment the market is busiest.

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>

namespace riskgate {

// Short string stored inline. Used for symbols, accounts and ClOrdIDs, which
// are all bounded in practice (FIX venues cap ClOrdID at 20-36 characters).
template <std::size_t N>
class FixedString {
    static_assert(N > 0 && N < 256, "length is stored in one byte");

public:
    FixedString() = default;

    [[nodiscard]] static std::optional<FixedString> from(std::string_view s) noexcept {
        if (s.size() > N) return std::nullopt;
        FixedString out;
        std::memcpy(out.data_, s.data(), s.size());
        out.len_ = static_cast<std::uint8_t>(s.size());
        return out;
    }

    [[nodiscard]] std::string_view view() const noexcept { return {data_, len_}; }
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    [[nodiscard]] bool empty() const noexcept { return len_ == 0; }

    friend bool operator==(const FixedString& a, const FixedString& b) noexcept {
        return a.view() == b.view();
    }

    static constexpr std::size_t capacity = N;

private:
    char         data_[N]{};
    std::uint8_t len_{0};
};

// FNV-1a, 64-bit. Short keys, no dependency, good enough distribution for a
// linear-probing table that is kept under half full.
[[nodiscard]] inline std::uint64_t hash_bytes(std::string_view s) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

// Open-addressing hash map keyed by short strings, linear probing with
// backward-shift deletion. Fixed capacity (rounded up to a power of two and
// doubled so the load factor stays <= 0.5). insert() returns nullptr when
// full: callers must fail closed (reject), never grow.
//
// Backward-shift deletion instead of tombstones matters for a gateway that
// runs all session: with tombstones, constant order churn slowly converts
// every empty slot into a tombstone and a lookup miss degrades to a scan of
// the whole table. Backward shift keeps probe chains exactly as short as the
// live entries require, forever.
template <std::size_t KeyLen, class Value>
class FlatStringMap {
    struct Slot {
        FixedString<KeyLen> key;
        Value               value{};
        std::uint32_t       hash{0};
        bool                used{false};
    };

public:
    explicit FlatStringMap(std::size_t max_entries)
        : cap_(std::bit_ceil(std::max<std::size_t>(max_entries * 2, 16))),
          mask_(cap_ - 1),
          max_entries_(max_entries),
          slots_(std::make_unique<Slot[]>(cap_)) {}

    [[nodiscard]] Value* find(std::string_view key) noexcept {
        const std::size_t i = locate(key);
        return i == kNotFound ? nullptr : &slots_[i].value;
    }

    [[nodiscard]] const Value* find(std::string_view key) const noexcept {
        const std::size_t i = locate(key);
        return i == kNotFound ? nullptr : &slots_[i].value;
    }

    // Inserts key -> value. Returns pointer to the stored value, or nullptr if
    // the key already exists, is too long, or the table is at capacity.
    Value* insert(std::string_view key, const Value& value) noexcept {
        if (key.size() > KeyLen || size_ >= max_entries_) return nullptr;
        const auto h = static_cast<std::uint32_t>(hash_bytes(key));
        for (std::size_t i = h & mask_;; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (!s.used) {
                s.key   = *FixedString<KeyLen>::from(key);
                s.value = value;
                s.hash  = h;
                s.used  = true;
                ++size_;
                return &s.value;
            }
            if (s.hash == h && s.key.view() == key) return nullptr;  // duplicate
        }
    }

    bool erase(std::string_view key) noexcept {
        std::size_t hole = locate(key);
        if (hole == kNotFound) return false;
        slots_[hole].used = false;
        --size_;
        // Pull later members of the probe chain back into the hole.
        for (std::size_t j = (hole + 1) & mask_; slots_[j].used; j = (j + 1) & mask_) {
            const std::size_t home = slots_[j].hash & mask_;
            // Move j into the hole unless j's home lies cyclically in (hole, j].
            const bool home_between = hole <= j ? (hole < home && home <= j)
                                                : (hole < home || home <= j);
            if (home_between) continue;
            slots_[hole] = slots_[j];
            slots_[j].used = false;
            hole = j;
        }
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t max_entries() const noexcept { return max_entries_; }
    [[nodiscard]] bool full() const noexcept { return size_ >= max_entries_; }

private:
    static constexpr std::size_t kNotFound = ~std::size_t{0};

    [[nodiscard]] std::size_t locate(std::string_view key) const noexcept {
        if (key.size() > KeyLen) return kNotFound;
        const auto h = static_cast<std::uint32_t>(hash_bytes(key));
        // Load factor <= 0.5 guarantees an empty slot terminates the probe.
        for (std::size_t i = h & mask_;; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (!s.used) return kNotFound;
            if (s.hash == h && s.key.view() == key) return i;
        }
    }

    std::size_t             cap_;
    std::size_t             mask_;
    std::size_t             max_entries_;
    std::size_t             size_{0};
    std::unique_ptr<Slot[]> slots_;
};

// Insert-only open-addressing map from a non-zero 64-bit key to a 32-bit
// index. Used for (account, symbol) -> position slot.
class FlatU64Index {
public:
    explicit FlatU64Index(std::size_t max_entries)
        : cap_(std::bit_ceil(std::max<std::size_t>(max_entries * 2, 16))),
          mask_(cap_ - 1),
          max_entries_(max_entries),
          keys_(std::make_unique<std::uint64_t[]>(cap_)),
          vals_(std::make_unique<std::uint32_t[]>(cap_)) {}

    // Returns the stored index or 0xFFFFFFFF if absent. Key 0 is reserved.
    [[nodiscard]] std::uint32_t find(std::uint64_t key) const noexcept {
        std::size_t i = mix(key) & mask_;
        for (std::size_t probes = 0; probes < cap_; ++probes, i = (i + 1) & mask_) {
            if (keys_[i] == 0) return 0xFFFF'FFFFu;
            if (keys_[i] == key) return vals_[i];
        }
        return 0xFFFF'FFFFu;
    }

    bool insert(std::uint64_t key, std::uint32_t value) noexcept {
        if (size_ >= max_entries_ || key == 0) return false;
        std::size_t i = mix(key) & mask_;
        for (std::size_t probes = 0; probes < cap_; ++probes, i = (i + 1) & mask_) {
            if (keys_[i] == key) return false;
            if (keys_[i] == 0) {
                keys_[i] = key;
                vals_[i] = value;
                ++size_;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    static std::uint64_t mix(std::uint64_t x) noexcept {  // splitmix64 finalizer
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27; x *= 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    std::size_t                      cap_;
    std::size_t                      mask_;
    std::size_t                      max_entries_;
    std::size_t                      size_{0};
    std::unique_ptr<std::uint64_t[]> keys_;
    std::unique_ptr<std::uint32_t[]> vals_;
};

// Fixed pool of T with an intrusive free list. acquire() returns kInvalidId
// when exhausted.
template <class T>
class SlotPool {
public:
    explicit SlotPool(std::uint32_t capacity)
        : cap_(capacity), items_(std::make_unique<T[]>(capacity)),
          next_free_(std::make_unique<std::uint32_t[]>(capacity)) {
        for (std::uint32_t i = 0; i < capacity; ++i) next_free_[i] = i + 1;
        free_head_ = 0;
    }

    [[nodiscard]] std::uint32_t acquire() noexcept {
        if (free_head_ >= cap_) return 0xFFFF'FFFFu;
        std::uint32_t idx = free_head_;
        free_head_ = next_free_[idx];
        items_[idx] = T{};
        ++in_use_;
        return idx;
    }

    void release(std::uint32_t idx) noexcept {
        assert(idx < cap_);
        next_free_[idx] = free_head_;
        free_head_ = idx;
        --in_use_;
    }

    [[nodiscard]] T&       operator[](std::uint32_t i) noexcept       { return items_[i]; }
    [[nodiscard]] const T& operator[](std::uint32_t i) const noexcept { return items_[i]; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return cap_; }
    [[nodiscard]] std::uint32_t in_use() const noexcept { return in_use_; }

private:
    std::uint32_t                    cap_;
    std::unique_ptr<T[]>             items_;
    std::unique_ptr<std::uint32_t[]> next_free_;
    std::uint32_t                    free_head_{0};
    std::uint32_t                    in_use_{0};
};

} // namespace riskgate
