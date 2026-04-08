# Phase 3: Lock-Free Execution Path

**Goal:** Remove all mutex locks and heap allocations from the order fill processing path. Currently, every simulated fill flows through `OrderExecutor::process_order_response()` -> `OrderManager::placeOrder()` -> `executeOrder()` -> `simulateOrderFill()` -> `updatePositionAndPnL()` (acquires `pnl_mutex_`) -> `updateSessionStats()` (acquires `session_mutex_`). That's two mutex acquisitions per filled order on the hot path.

**Target improvement:** ~25-100+ ns per fill. Eliminates all mutex syscalls and all `std::string` heap allocations from the fill path.

**Dependencies:** Phase 1 (foundation fixes) and Phase 2 (simdjson -- separate concern but should be done first for clean measurements).

---

## Change 1: Replace OrderResponse Strings with Enums and Fixed-Size Buffers

**File:** `include/order/order_manager.h` lines 10-17

**Problem:** `OrderResponse` contains three `std::string` fields -- `order_id`, `status`, and `error_message`. Each `std::string` is a potential heap allocation (unless SSO kicks in for short strings). This struct is constructed and returned by value on every fill, generating up to 3 allocations per call.

**Before:**
```cpp
struct OrderResponse {
    bool success = false;
    std::string order_id;
    std::string status;
    std::string error_message;
    double filled_quantity = 0.0;
    double avg_fill_price = 0.0;
};
```

**After:**
```cpp
enum class FillStatus : uint8_t {
    SUCCESS,
    INVALID_PARAMS,
    RISK_REJECTED,
    INTERNAL_ERROR
};

struct OrderResponse {
    FillStatus status = FillStatus::INTERNAL_ERROR;
    uint64_t order_id = 0;
    double filled_quantity = 0.0;
    double avg_fill_price = 0.0;

    bool success() const { return status == FillStatus::SUCCESS; }
};
```

Changes:
- `success` boolean replaced by `FillStatus` enum -- carries the same information plus reason for failure
- `order_id` changed from `std::string` to `uint64_t` -- the executor already uses integer order IDs (`generate_order_id()` returns `uint64_t`)
- `status` string replaced by the enum
- `error_message` removed -- the `FillStatus` enum encodes the reason; if detailed error reporting is needed for debugging, it can be logged separately (off the hot path)

**Callers to update:**
- `src/execution/executor.cpp` line 72: `if (!result.success)` -> `if (!result.success())`
- Any place that reads `result.order_id` as string, `result.status` as string, or `result.error_message`

**Risk:** Low. The string fields were never used for anything other than logging in the session summary. The enum provides the same branching capability.

---

## Change 2: Change placeOrder to Accept string_view

**File:** `include/order/order_manager.h` line 27, `src/order/order_manager.cpp` line 31

**Problem:** `placeOrder` takes `const std::string& symbol`. The executor calls it with `response.symbol.data()` (a `const char*` from `std::array<char, 16>`), which implicitly constructs a temporary `std::string` -- a heap allocation on every fill.

**Before:**
```cpp
OrderResponse placeOrder(const std::string& symbol, Side side, double price, double quantity);
```

**After:**
```cpp
#include <string_view>

OrderResponse placeOrder(std::string_view symbol, Side side, double price, double quantity);
```

Also update `executeOrder`, `validateOrder`, and all internal methods that forward the symbol to accept `std::string_view`. When the symbol needs to be stored in the `Order` struct, it can be copied at that point (see Change 4 for eliminating that copy too).

**Call site change (`executor.cpp` line 69-70):**
```cpp
// Before:
auto result = order_manager_.placeOrder(
    response.symbol.data(), side, response.price, response.filled_quantity);

// After (identical call, but now constructs string_view instead of string):
auto result = order_manager_.placeOrder(
    response.symbol.data(), side, response.price, response.filled_quantity);
```

The call site doesn't change, but the implicit conversion now creates a `string_view` (pointer + size, stack-only) instead of a `std::string` (heap allocation).

**Risk:** Low. `string_view` is a drop-in replacement for `const string&` in read-only contexts. The only danger is if the viewed data goes out of scope before the `string_view` is used -- but in this case, the `response` outlives the `placeOrder` call.

---

## Change 3: Replace pnl_mutex_ with Lock-Free Atomic PnL Tracking

**File:** `include/order/order_manager.h` lines 34-37, `src/order/order_manager.cpp` lines 119-158

**Problem:** `updatePositionAndPnL()` acquires `pnl_mutex_` on every fill. This is called from the order engine hot path. Even uncontended, a mutex lock/unlock is ~25ns (a futex syscall on Linux). Under contention (risk thread or metrics thread reading position/PnL), it can spike to microseconds.

**Approach:** Replace the mutex-protected PnL state with a lock-free design using compare-exchange loops on atomics.

The PnL calculation is complex (it needs `current_position_`, `avg_entry_price_`, and `cumulative_pnl_` atomically), so a simple atomic swap won't work. Two viable approaches:

### Option A: Atomic PnL with CAS Loop (Recommended)

Pack the three PnL fields into a struct and use a seqlock pattern for atomicity:

```cpp
struct PnLState {
    double position = 0.0;
    double avg_entry_price = 0.0;
    double cumulative_pnl = 0.0;
};

// In OrderManager:
alignas(64) std::atomic<uint64_t> pnl_seq_{0};  // Seqlock sequence counter
PnLState pnl_state_;  // Written only by order engine thread

// Writer (hot path -- single writer, no contention):
void updatePositionAndPnL(const Order& order) {
    pnl_seq_.fetch_add(1, std::memory_order_release);  // Begin write

    // ... compute new position, avg_entry_price, cumulative_pnl ...
    // (same logic as current updatePositionAndPnL, operating on pnl_state_ directly)
    pnl_state_.position = new_position;
    pnl_state_.avg_entry_price = new_avg;
    pnl_state_.cumulative_pnl += realized_pnl;

    pnl_seq_.fetch_add(1, std::memory_order_release);  // End write
}

// Reader (risk thread, metrics thread -- may retry):
PnLState readPnLState() const {
    PnLState snapshot;
    uint64_t seq;
    do {
        seq = pnl_seq_.load(std::memory_order_acquire);
        if (seq & 1) continue;  // Write in progress, spin

        snapshot.position = pnl_state_.position;
        snapshot.avg_entry_price = pnl_state_.avg_entry_price;
        snapshot.cumulative_pnl = pnl_state_.cumulative_pnl;

        std::atomic_thread_fence(std::memory_order_acquire);
    } while (pnl_seq_.load(std::memory_order_relaxed) != seq);

    return snapshot;
}
```

This is a classic seqlock -- the writer is wait-free (no contention, no CAS retry), and readers retry only if they catch a write in progress (extremely rare since the write is ~10 instructions).

**Critical requirement:** There must be exactly ONE writer thread. The current architecture satisfies this -- only `process_order_response()` on the order engine thread calls `updatePositionAndPnL()`.

### Option B: Simpler -- Separate Hot and Cold Paths

If the seqlock feels too complex, a simpler approach: keep `pnl_mutex_` but make the hot path avoid it entirely:

- Hot path (`updatePositionAndPnL`): Write to atomic-only fields (`std::atomic<double> position_`, `std::atomic<double> pnl_`). Use `compare_exchange_weak` for the update.
- Cold path (readers): Read the atomics directly. No mutex needed.
- Session stats: Accumulate via a separate lock-free counter.

The downside is that `avg_entry_price` would need its own atomic and the computation becomes harder to do atomically (it requires reading both position and avg_entry_price consistently). The seqlock solves this more cleanly.

**Update reader call sites:**
- `src/order/order_manager.cpp:41` (`getCurrentPnL`): Use `readPnLState().cumulative_pnl`
- `src/order/order_manager.cpp:46` (`getCurrentPosition`): Use `readPnLState().position`
- `src/metrics/metrics.cpp:22-23`: Use `readPnLState()`
- `src/engine.cpp:174,206`: Use `readPnLState()`

**Risk:** Medium. Seqlocks are well-understood in HFT but require careful memory ordering. The single-writer constraint must be maintained -- if the architecture ever adds a second writer, this breaks. Document the invariant clearly.

---

## Change 4: Move Session Stats Off the Hot Path

**File:** `src/order/order_manager.cpp` lines 109-117 (simulateOrderFill), 170-180 (updateSessionStats)

**Problem:** `updateSessionStats()` acquires `session_mutex_` on every fill to increment `buy_trades_`, `sell_trades_`, `total_buy_volume_`, `total_sell_volume_`. This is a second mutex acquisition per fill, purely for bookkeeping that's only read at shutdown.

**Approach:** Replace session stats with atomic counters (no mutex needed for simple increments):

```cpp
// Replace session_mutex_-protected fields with atomics:
std::atomic<uint64_t> buy_trades_{0};
std::atomic<uint64_t> sell_trades_{0};
std::atomic<double> total_buy_volume_{0.0};
std::atomic<double> total_sell_volume_{0.0};

void updateSessionStats(Side side, double quantity) {
    if (side == Side::BUY) {
        buy_trades_.fetch_add(1, std::memory_order_relaxed);
        // For atomic<double> add, use CAS loop:
        double old_vol = total_buy_volume_.load(std::memory_order_relaxed);
        while (!total_buy_volume_.compare_exchange_weak(
            old_vol, old_vol + quantity, std::memory_order_relaxed)) {}
    } else {
        sell_trades_.fetch_add(1, std::memory_order_relaxed);
        double old_vol = total_sell_volume_.load(std::memory_order_relaxed);
        while (!total_sell_volume_.compare_exchange_weak(
            old_vol, old_vol + quantity, std::memory_order_relaxed)) {}
    }
}
```

With a single writer thread, the CAS loops will never actually retry (no contention), so this is effectively a single atomic store per counter.

Alternatively, since there's only one writer: just use non-atomic fields with the seqlock from Change 3. Bundle session stats into the same seqlock-protected region.

**Risk:** Low. Session stats are purely informational and tolerate slight staleness.

---

## Change 5: Eliminate std::string from Hot-Path Order Struct

**File:** `include/core/types.h` lines 15-27

**Problem:** The `Order` struct used by `OrderManager` contains three `std::string` fields: `order_id`, `symbol`, and `client_order_id`. Each `placeOrder` call constructs an `Order` with these strings, triggering ~5 heap allocations (3 strings + 2 from `generateClientOrderId`'s `std::to_string` concatenations).

**Before:**
```cpp
struct Order {
    std::string order_id;
    std::string symbol;
    Side side;
    OrderType type;
    double price;
    double quantity;
    double filled_quantity = 0.0;
    OrderStatus status = OrderStatus::PENDING;
    std::chrono::system_clock::time_point create_time;
    std::chrono::system_clock::time_point update_time;
    std::string client_order_id;
};
```

**After:**
```cpp
struct Order {
    uint64_t order_id = 0;
    std::array<char, 16> symbol{};
    Side side = Side::BUY;
    OrderType type = OrderType::LIMIT;
    double price = 0.0;
    double quantity = 0.0;
    double filled_quantity = 0.0;
    OrderStatus status = OrderStatus::PENDING;
    std::chrono::system_clock::time_point create_time;
    std::chrono::system_clock::time_point update_time;
    uint64_t client_order_id = 0;
};
```

This matches the `HFTOrder` struct pattern (which already uses `uint64_t` for IDs and `std::array<char, 16>` for symbol).

**Also remove `generateClientOrderId()`** (order_manager.cpp lines 50-60) -- it does `std::to_string` concatenations to produce a string ID. Replace with:
```cpp
uint64_t generateOrderId() {
    return next_order_id_.fetch_add(1, std::memory_order_relaxed);
}
```

**Callers to update:**
- `executeOrder()`: Use integer ID directly
- `generateSessionSummary()`: Use integer IDs for logging (convert to string only at print time, which is off the hot path)

**Risk:** Low-medium. If any external interface expects string order IDs, a `std::to_string` conversion can be done at the boundary (off the hot path). Internal hot-path code should only deal with integers.

---

## Change 6: Add Move Semantics to SPSC Queue

**File:** `include/core/spsc_queue.h`

**Problem:** The queue only supports `push(const T& item)`, which copies the item. For `HFTOrder` (~104 bytes with three `time_point` fields), the copy is a memcpy and is fine for POD types. But if any queue element type has non-trivial move semantics (e.g., after future changes), the copy becomes expensive.

**After:** Add move-push support:
```cpp
bool push(const T& item) {
    const auto current_tail = tail_.load(std::memory_order_relaxed);
    const auto next_tail = increment(current_tail);
    if (next_tail == head_.load(std::memory_order_acquire)) {
        return false;
    }
    buffer_[current_tail] = item;
    tail_.store(next_tail, std::memory_order_release);
    return true;
}

bool push(T&& item) {
    const auto current_tail = tail_.load(std::memory_order_relaxed);
    const auto next_tail = increment(current_tail);
    if (next_tail == head_.load(std::memory_order_acquire)) {
        return false;
    }
    buffer_[current_tail] = std::move(item);
    tail_.store(next_tail, std::memory_order_release);
    return true;
}

template<typename... Args>
bool emplace(Args&&... args) {
    const auto current_tail = tail_.load(std::memory_order_relaxed);
    const auto next_tail = increment(current_tail);
    if (next_tail == head_.load(std::memory_order_acquire)) {
        return false;
    }
    new (&buffer_[current_tail]) T(std::forward<Args>(args)...);
    tail_.store(next_tail, std::memory_order_release);
    return true;
}
```

**Risk:** Low. The move overload is additive -- existing code calling `push(const T&)` continues to work.

---

## Change 7: Seqlock for Shared Market Data (BBO)

**File:** `include/data/market_data.h` lines 45-48, `src/data/market_data_feed.cpp` lines 108-114

**Problem:** The MarketDataFeed writes 5 atomic values every tick (bid, ask, spread, update timestamp, and the latency metric). Each `store()` with `seq_cst` generates a full memory fence. The order engine reads bid/ask via `market_data_feed_->bid()` and `market_data_feed_->ask()` as separate atomic loads, which means the bid and ask could come from different ticks (torn read).

**After:** Use a seqlock to share a BBO snapshot atomically with zero fences on the write side beyond the seqlock increments:

```cpp
struct BBOSnapshot {
    double bid_price = 0.0;
    double ask_price = 0.0;
    double bid_qty = 0.0;
    double ask_qty = 0.0;
    double spread_bps = 0.0;
    uint64_t timestamp_ns = 0;
};

// In MarketDataFeed:
alignas(64) std::atomic<uint64_t> bbo_seq_{0};
BBOSnapshot bbo_;  // Written by WS thread only

// Writer (WS callback thread):
void publishBBO(double bid, double ask, double bid_qty, double ask_qty,
                uint64_t ts_ns) {
    bbo_seq_.fetch_add(1, std::memory_order_release);
    bbo_.bid_price = bid;
    bbo_.ask_price = ask;
    bbo_.bid_qty = bid_qty;
    bbo_.ask_qty = ask_qty;
    bbo_.spread_bps = ((ask - bid) / bid) * 10000.0;
    bbo_.timestamp_ns = ts_ns;
    bbo_seq_.fetch_add(1, std::memory_order_release);
}

// Reader (order engine thread):
BBOSnapshot readBBO() const {
    BBOSnapshot snap;
    uint64_t seq;
    do {
        seq = bbo_seq_.load(std::memory_order_acquire);
        if (seq & 1) continue;
        snap = bbo_;
        std::atomic_thread_fence(std::memory_order_acquire);
    } while (bbo_seq_.load(std::memory_order_relaxed) != seq);
    return snap;
}
```

This replaces 5 individual atomic stores with 2 atomic increments + plain stores, and replaces 2 individual atomic loads with a single consistent snapshot read.

**Risk:** Medium. Same single-writer constraint as Change 3. Must be documented.

---

## Verification

After applying all Phase 3 changes:

1. **Build:** Verify compilation with no warnings.
2. **Smoke test:** The smoke test must be updated for new `OrderResponse` shape and `Order` struct changes. All assertions must still pass.
3. **Correctness test:** Run the bot for 60s and verify:
   - PnL calculations match (compare to pre-refactor run with same seed if possible)
   - Session summary reports correct trade counts and volumes
   - No crashes or data corruption (run under AddressSanitizer: `-fsanitize=address`)
4. **ThreadSanitizer:** Run under TSan (`-fsanitize=thread`) to verify the seqlock implementations are free of data race reports. Note: TSan may flag seqlocks as races -- this is a known false positive. Use `__attribute__((no_sanitize("thread")))` annotations on seqlock read/write functions if needed.

---

## Complexity Summary

| Change | Effort | Risk | Impact |
|--------|--------|------|--------|
| 1. OrderResponse enums | Low | Low | Medium (~50-150ns/fill) |
| 2. string_view for placeOrder | Low | Low | Medium (~30-80ns/fill) |
| 3. Seqlock for PnL | Medium | Medium | High (~25-1000ns/fill) |
| 4. Atomic session stats | Low | Low | Medium (~25ns/fill) |
| 5. Fixed-size Order struct | Medium | Low-Med | High (~150-300ns/fill) |
| 6. Move semantics for SPSC | Low | Low | Low (future-proofing) |
| 7. Seqlock for BBO | Medium | Medium | Medium (~30-60ns/tick) |
