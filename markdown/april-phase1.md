# Phase 1: Foundation and Quick Wins

**Goal:** Eliminate the worst low-hanging-fruit latency killers with minimal code churn. No architectural changes -- just fixing data layout, memory ordering, and removing unnecessary waits.

**Target improvement:** ~10-30 us off hot-path processing latency. Eliminates 10-60ms of WebSocket poll jitter.

**Dependencies:** None -- this is the starting phase.

---

## Change 1: Fix SPSC Queue False Sharing

**File:** `include/core/spsc_queue.h` lines 35-36

**Problem:** `head_` and `tail_` are adjacent `std::atomic<size_t>` fields that share a cache line (64 bytes on x86). The producer writes `tail_` and the consumer writes `head_`, causing the MESI protocol to bounce the cache line between cores on every push/pop. This adds ~50-100ns per operation.

**Before:**
```cpp
std::atomic<size_t> head_{0};
std::atomic<size_t> tail_{0};
std::array<T, Size> buffer_;
```

**After:**
```cpp
alignas(64) std::atomic<size_t> head_{0};
char pad_head_[64 - sizeof(std::atomic<size_t>)];

alignas(64) std::atomic<size_t> tail_{0};
char pad_tail_[64 - sizeof(std::atomic<size_t>)];

alignas(64) std::array<T, Size> buffer_;
```

**Risk:** Low. Pure data layout change, no logic change.

---

## Change 2: Replace Modulo with Bitmask in SPSC Queue

**File:** `include/core/spsc_queue.h` line 40

**Problem:** `(idx + 1) % Size` compiles to a division instruction (~20-40 cycles on x86). Called on every push and pop.

**Before:**
```cpp
size_t increment(size_t idx) const {
    return (idx + 1) % Size;
}
```

**After:**
```cpp
static_assert((Size & (Size - 1)) == 0, "SPSCQueue size must be a power of two");

size_t increment(size_t idx) const {
    return (idx + 1) & (Size - 1);
}
```

Also update queue instantiations to power-of-2 sizes. Current usages:
- `SPSCQueue<HFTMarketData, 1024>` in `include/engine.h` line 46 -- already power of 2.
- `SPSCQueue<HFTOrder, 2048>` in `include/execution/executor.h` line 52 -- already power of 2.

**Risk:** Low. The existing sizes are already powers of two. The `static_assert` catches violations at compile time.

---

## Change 3: Remove 10ms Sleep from WebSocket Worker Loop

**File:** `src/data/websocket_client.cpp` line 182

**Problem:** After `lws_service(context_, 50)` returns (which itself can block up to 50ms), there's an unconditional 10ms sleep. This means every market data message sits in the kernel socket buffer for an additional 10ms before being processed. For an HFT system, this is a 10-60ms latency floor on the entire data path.

**Before:**
```cpp
while (running_.load()) {
    int result = lws_service(context_, 50);
    if (result < 0) {
        std::cout << "lws_service error, terminating worker loop" << std::endl;
        break;
    }
    flushTxQueue();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
```

**After:**
```cpp
while (running_.load()) {
    int result = lws_service(context_, 1);
    if (result < 0) {
        std::cout << "lws_service error, terminating worker loop" << '\n';
        break;
    }
    flushTxQueue();
}
```

Changes:
- Remove the 10ms sleep entirely.
- Reduce `lws_service` timeout from 50ms to 1ms so it returns quickly when there's no data, keeping the loop responsive.

**Risk:** Low-medium. Increases CPU usage on the WebSocket thread (it will poll more aggressively). This is the desired behavior for an HFT system -- you're trading CPU cycles for latency. If power consumption is a concern on dev machines, this can be made configurable.

---

## Change 4: Kill the Useless market_data_worker Thread

**File:** `src/engine.cpp` lines 110-117, `include/engine.h` line 41

**Problem:** `market_data_worker()` calls `market_data_feed_->start()` (which just registers a callback), then does nothing but sleep in a 100ms loop. The actual data processing happens on the WebSocket worker thread via the callback. This is a wasted thread and a wasted OS resource.

**Before (engine.cpp):**
```cpp
void HFTEngine::market_data_worker() {
    logger_->info("Market data worker started");
    market_data_feed_->start(trading_symbol_, market_data_queue_);

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
```

**After:** Move the `start()` call into `HFTEngine::start()`, directly before launching threads. Remove `market_data_worker()` entirely. Remove `market_data_thread_` from the header and all join/launch sites.

```cpp
void HFTEngine::start() {
    if (running_.load()) return;
    running_.store(true);

    std::string ws_url = Config::getInstance().getCoinbaseWsUrl();
    if (!websocket_client_->connect(ws_url)) {
        logger_->error("Failed to connect WebSocket for market data");
        running_.store(false);
        return;
    }

    websocket_client_->subscribeOrderBook(trading_symbol_, 10, 100);
    market_data_feed_->start(trading_symbol_, market_data_queue_);

    order_engine_thread_ = std::thread(&HFTEngine::order_engine_worker, this);
    risk_thread_ = std::thread(&HFTEngine::risk_management_worker, this);
    metrics_thread_ = std::thread(&HFTEngine::metrics_worker, this);

    // ... rest unchanged
}
```

**Risk:** Low. The thread was doing nothing useful. The callback registration has no threading requirements.

---

## Change 5: Switch Metrics Atomics to memory_order_relaxed

**Files:**
- `src/data/market_data_feed.cpp` lines 96, 108-114, 117
- `src/execution/executor.cpp` lines 42, 53, 74, 80, 88, 91, 93
- `src/metrics/metrics.cpp` lines 28, 72, 78

**Problem:** All atomic stores/loads for metrics counters use the default `memory_order_seq_cst`, which generates full memory fences (`MFENCE` on x86, ~10ns each). Metrics consumers (the metrics thread) tolerate stale reads, so sequential consistency is unnecessary overhead.

**Changes:** Add `std::memory_order_relaxed` to all metrics atomic operations:
- `metrics_.websocket_latency_ns.store(display_latency_ns)` -> `store(..., std::memory_order_relaxed)`
- `current_bid_.store(best_bid)` -> `store(..., std::memory_order_relaxed)`
- `current_ask_.store(best_ask)` -> `store(..., std::memory_order_relaxed)`
- `current_spread_bps_.store(...)` -> `store(..., std::memory_order_relaxed)`
- `last_market_update_.store(...)` -> `store(..., std::memory_order_relaxed)`
- `metrics_.market_data_updates.fetch_add(1)` -> `fetch_add(1, std::memory_order_relaxed)`
- `metrics_.orders_placed.fetch_add(1)` -> `fetch_add(1, std::memory_order_relaxed)`
- `metrics_.orders_filled.fetch_add(1)` -> `fetch_add(1, std::memory_order_relaxed)`
- `metrics_.total_pnl.store(...)` -> `store(..., std::memory_order_relaxed)`
- All corresponding `.load()` calls in `metrics.cpp` -> `load(std::memory_order_relaxed)`

Also apply relaxed ordering to `running_.load()` in hot loops (`engine.cpp` line 126, `websocket_client.cpp` line 175) -- the engine only sets this once at shutdown, so relaxed is safe (worst case: one extra iteration).

**Risk:** Low. Relaxed ordering is correct here because metrics are statistical -- a stale read by 1 iteration is acceptable.

---

## Change 6: Cache Atomic Values Locally in Hot Loops

**File:** `src/execution/executor.cpp` lines 36, 47, 88, 91, 93

**Problem:** `risk_breach_.load()` is called twice per ladder level (once for bid, once for ask), and `current_position_.load()` is called in `check_risk_limits` for every order. With 5 levels, that's 10 `risk_breach_` loads and 10 `current_position_` loads per signal. These values change rarely (position changes on fills, risk_breach changes on risk thread updates).

**Before:**
```cpp
for (uint32_t level = 0; level < signal.num_levels; ++level) {
    if (signal.place_bid && !risk_breach_.load()) { ... }
    if (signal.place_ask && !risk_breach_.load()) { ... }
}
```

**After:**
```cpp
const bool breached = risk_breach_.load(std::memory_order_relaxed);
const double pos = current_position_.load(std::memory_order_relaxed);
const double max_pos = max_position_.load(std::memory_order_relaxed);

for (uint32_t level = 0; level < signal.num_levels; ++level) {
    if (signal.place_bid && !breached) { ... }
    if (signal.place_ask && !breached) { ... }
}
```

Similarly, pass cached `pos` and `max_pos` into `check_risk_limits` instead of loading inside the function.

Same treatment for `order_engine_worker` in `engine.cpp` lines 126-164: cache `order_size_` at the top of each iteration or on the timer path only.

**Risk:** Low. Position may be stale by one ladder execution -- negligible for risk purposes.

---

## Change 7: Remove string::find("error") Scan on Every WebSocket Message

**File:** `src/data/websocket_client.cpp` lines 220-230

**Problem:** Every incoming WebSocket message (including all L2 order book data) does a linear scan `message.find("error")` on the raw string. This is O(n) on message length for a condition that's virtually never true during normal operation. The scan runs on the WebSocket thread, blocking all market data processing.

**Before:**
```cpp
if (message.find("error") != std::string::npos) {
    std::cout << "ERROR MESSAGE RECEIVED:" << std::endl;
    // ...
}
```

**After:** Check the parsed JSON object instead -- only for messages that aren't L2 data:
```cpp
if (json_msg.contains("type") && json_msg["type"] == "error") {
    std::cerr << "ERROR MESSAGE RECEIVED: " << message.substr(0, 500) << '\n';
}
```

Even better: defer error checking to the callback. If the callback doesn't recognize the message as L2 data, it can check for errors there. The hot path should do zero work on error detection.

**Risk:** Low. Error messages from Coinbase have a distinct `"type": "error"` field. Checking the JSON field is both more correct and faster for the common case.

---

## Change 8: Replace yield() with _mm_pause() Tiered Spin

**File:** `src/engine.cpp` line 162

**Problem:** `std::this_thread::yield()` is a `sched_yield()` system call that relinquishes the CPU time slice and puts the thread at the back of the scheduler queue. When the next market data message arrives, there's a 1-15 microsecond re-scheduling delay before the thread wakes up.

**Before:**
```cpp
if (!did_work) {
    std::this_thread::yield();
}
```

**After:** Use a tiered spin strategy with `_mm_pause()`:
```cpp
#include <immintrin.h>

// In the loop:
if (!did_work) {
    for (int i = 0; i < 32; ++i) {
        _mm_pause();
    }
}
```

`_mm_pause()` (the x86 `PAUSE` instruction) is ~5ns, doesn't leave the core, and hints the CPU to reduce power consumption during the spin. 32 iterations is ~160ns of waiting -- long enough to avoid a tight spin but short enough that the thread is immediately ready when data arrives.

For a more adaptive version that backs off on sustained idle:
```cpp
static int idle_count = 0;
if (!did_work) {
    if (++idle_count < 1000) {
        for (int i = 0; i < 32; ++i) _mm_pause();
    } else {
        std::this_thread::yield();
    }
} else {
    idle_count = 0;
}
```

**Risk:** Low-medium. Increases CPU usage when idle (the core stays active). This is standard practice for HFT -- you dedicate cores to trading threads and accept 100% utilization. On a dev laptop, the tiered approach with yield() fallback is more appropriate.

---

## Change 9: Cache-Line Align Cross-Thread Shared Atomics

**File:** `include/engine.h` lines 40-51, `include/metrics/metrics.h` lines 11-22

**Problem:** Several atomics that are accessed from different threads are packed adjacently, causing false sharing. In `engine.h`: `running_`, `order_size_`, `max_position_`, `current_position_`, and `risk_breach_` are all adjacent and accessed by different threads (order engine, risk thread, metrics thread).

**Before (engine.h):**
```cpp
std::atomic<bool> running_{false};
// ...
std::atomic<double> order_size_{0.005};
std::atomic<double> max_position_{0.1};
std::atomic<double> current_position_{0.0};
std::atomic<bool> risk_breach_{false};
```

**After:**
```cpp
alignas(64) std::atomic<bool> running_{false};
alignas(64) std::atomic<double> current_position_{0.0};
alignas(64) std::atomic<bool> risk_breach_{false};

// Rarely changed -- can share a cache line
std::atomic<double> order_size_{0.005};
std::atomic<double> max_position_{0.1};
```

The key insight: `current_position_` is written by the order engine thread (CAS loop on every fill) and read by the risk thread every 100ms. `risk_breach_` is written by the risk thread and read by the order engine thread every iteration. These two must be on separate cache lines.

Apply the same treatment to `AtomicHFTMetrics` in `include/metrics/metrics.h` -- fields written by different threads should be on separate cache lines.

**Risk:** Low. Increases struct size by ~200-300 bytes. Negligible memory cost.

---

## Change 10: Add likely/unlikely Branch Hints

**Files:** Various hot-path files

**Problem:** Error-handling branches on the hot path (JSON parse failures, risk breaches, empty books) are taken <0.1% of the time but the compiler doesn't know this, leading to suboptimal branch layout.

**Macro:**
```cpp
#define HFT_LIKELY(x)   __builtin_expect(!!(x), 1)
#define HFT_UNLIKELY(x) __builtin_expect(!!(x), 0)
```

**Key applications:**
- `engine.cpp` line 131: `if (HFT_LIKELY(market_data_queue_.pop(market_data)))` -- the queue usually has data
- `executor.cpp` line 66: `if (HFT_UNLIKELY(response.status != 'F')) return;`
- `executor.cpp` line 72: `if (HFT_UNLIKELY(!result.success)) return;`
- `executor.cpp` line 88: `if (HFT_UNLIKELY(risk_breach_.load())) return false;`
- `market_data_feed.cpp` line 31: `if (HFT_UNLIKELY(!message.contains("channel") || ...))` -- guard checks
- `market_data_feed.cpp` line 88: `if (HFT_UNLIKELY(best_bid >= best_ask)) continue;` -- crossed book is rare
- `websocket_client.cpp` line 282: `if (HFT_UNLIKELY(!client_instance || ...))` -- null check

Define the macros in a new header or at the top of `include/core/types.h`.

**Risk:** Very low. These are compiler hints only -- they affect code layout, not correctness. Impact is ~0.5-3% on tight loops.

---

## Verification

After applying all Phase 1 changes:

1. **Build:** `cmake --build build` must succeed with no warnings.
2. **Smoke test:** `./build/smoke_test` must pass all assertions.
3. **Run test:** Start the bot for 30s and verify:
   - WebSocket connects and receives data (no 10ms jitter visible in logs).
   - Order engine processes signals and fills.
   - Session summary reports reasonable metrics.
4. **Latency measurement:** Compare `avg_order_latency_ns` before and after. Expect a measurable improvement (the metric captures `place_order_ladder` time, which benefits from cached atomics and relaxed ordering).

---

## Complexity Summary

| Change | Effort | Risk | Impact |
|--------|--------|------|--------|
| 1. SPSC false sharing fix | Low | Low | High (~50-100ns/op) |
| 2. Bitmask in SPSC | Low | Low | Medium (~20ns/op) |
| 3. Remove WS 10ms sleep | Low | Low | Very High (10-60ms) |
| 4. Kill dead thread | Low | Low | Low (cleanup) |
| 5. Relaxed memory ordering | Low | Low | Medium (~5-10ns/atomic) |
| 6. Cache atomics locally | Low | Low | Medium (~50ns/ladder) |
| 7. Remove error string scan | Low | Low | Medium (~100ns/msg) |
| 8. _mm_pause spin | Low | Low-Med | Medium (~1-15us wake) |
| 9. Cache-line align atomics | Low | Low | Medium (~50ns/access) |
| 10. Branch hints | Low | Very Low | Low (~0.5-3%) |
