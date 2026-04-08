# Phase 4: Threading Model Overhaul

**Goal:** Restructure the threading model to minimize inter-thread communication latency. The current architecture spreads the hot path across 3 threads (WebSocket worker -> SPSC queue -> order engine) with synchronization overhead at each boundary. This phase consolidates the critical path, pins threads to cores, and moves non-critical work off the hot path.

**Target improvement:** ~1-5 us reduction in inter-thread synchronization latency. Significant reduction in tail latency jitter from OS scheduler thread migration.

**Dependencies:** Phase 1, 2, and 3 (the hot path must already be allocation-free and lock-free before threading changes will have measurable impact).

---

## Change 1: Merge Feed Parsing + Strategy + Execution onto a Single Thread

**Files:** `src/engine.cpp`, `include/engine.h`, `src/data/market_data_feed.cpp`

**Problem:** The current architecture has a pipeline:

```
WS Thread: lws_service() -> callback -> parse JSON -> update book -> push to SPSC queue
Order Engine Thread: pop from SPSC queue -> generate signal -> place ladder -> process fills
```

The SPSC queue adds ~50-100ns per message (even with Phase 1 false-sharing fix). More critically, the order engine thread must wake up from a spin/yield to process the message, adding 100-1000ns of wakeup latency depending on the spin strategy.

**Approach:** Run the strategy and execution directly in the WebSocket callback, eliminating the SPSC queue entirely:

```
WS Thread: lws_service() -> callback -> parse -> update book -> generate signal -> place ladder
```

The order engine thread becomes the WebSocket thread. This is the standard architecture for top-tier HFT systems -- a single thread handles the entire tick-to-trade pipeline.

**Implementation sketch:**

```cpp
// In MarketDataFeed callback (after updating book and computing BBO):
void MarketDataFeed::processMessage(const char* data, size_t len) {
    // ... parse, update book, compute BBO ...

    if (bid_count_ > 0 && ask_count_ > 0) {
        double best_bid = bid_levels_[0].price;
        double best_ask = ask_levels_[0].price;
        if (best_bid < best_ask) {
            // Instead of pushing to SPSC queue, call strategy directly:
            if (strategy_callback_) {
                strategy_callback_(best_bid, best_ask, callback_context_);
            }
        }
    }
}

// The strategy callback runs the full pipeline:
static void on_market_update(double bid, double ask, void* ctx) {
    auto* engine = static_cast<HFTEngine*>(ctx);
    double pos = engine->current_position_.load(std::memory_order_relaxed);
    double size = engine->order_size_.load(std::memory_order_relaxed);

    HFTSignal signal = engine->strategy_->generate_signal(bid, ask, pos, size);
    if (signal.place_bid || signal.place_ask) {
        engine->executor_->place_order_ladder(signal);
    }

    // Process fill responses from the executor's inbound queue:
    HFTOrder response{};
    while (engine->executor_->pop_response(response)) {
        engine->executor_->process_order_response(response);
    }
}
```

**What this eliminates:**
- SPSC queue push/pop overhead (~100ns per message)
- Inter-thread wakeup latency (~100-1000ns)
- The dedicated `order_engine_thread_` entirely

**What stays on separate threads:**
- Risk management worker (100ms polling -- keep it separate, it's not latency-critical)
- Metrics worker (1s polling -- keep it separate)

**The SPSC queue and the timer-based signal generation (`target_interval` path)** are removed. The strategy fires on every market data update, which is the correct HFT behavior -- you want to react to every tick, not on a fixed timer.

If rate-limiting is needed (to avoid over-trading), implement it inside `place_order_ladder` with a simple timestamp check (purely local, no cross-thread coordination).

**Risk:** High. This is a significant architectural change. The WebSocket callback now does more work per invocation, which affects `lws_service` throughput. If the strategy + executor take too long (unlikely with Phase 2-3 optimizations in place), messages could back up in the kernel socket buffer. Benchmark to verify.

---

## Change 2: CPU Pinning

**Files:** `src/engine.cpp` (thread launch), new utility `include/core/cpu_affinity.h`

**Problem:** The OS scheduler can migrate threads between CPU cores at any time. When a thread moves to a different core, its L1/L2 cache is cold, adding ~1-10us of cache-warming latency. For HFT, this jitter is unacceptable.

**Implementation:**
```cpp
// include/core/cpu_affinity.h
#pragma once
#include <cstdint>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

inline bool pin_thread_to_core(int core_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(),
        sizeof(cpuset), &cpuset) == 0;
#elif defined(__APPLE__)
    // macOS doesn't support hard affinity; use affinity tags as hints
    thread_affinity_policy_data_t policy = { core_id + 1 };
    return thread_policy_set(
        pthread_mach_thread_np(pthread_self()),
        THREAD_AFFINITY_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#else
    (void)core_id;
    return false;
#endif
}
```

**Usage in engine.cpp:**
```cpp
// After thread launch, pin to specific cores:
// Core 0: OS + misc
// Core 1: WebSocket/trading thread (hot path)
// Core 2: Risk management
// Core 3: Metrics + logging

// In the WS worker thread (if using Change 1's merged model):
void WebSocketClient::workerLoop() {
    pin_thread_to_core(1);
    // ... existing loop ...
}

// In risk_management_worker:
void HFTEngine::risk_management_worker() {
    pin_thread_to_core(2);
    // ... existing loop ...
}
```

**Configuration:** Add `TRADING_CORE`, `RISK_CORE`, `METRICS_CORE` to `config.txt` so the user can configure core assignments based on their hardware. Default to -1 (no pinning) for dev environments.

**Risk:** Low-medium. On Linux, this provides hard guarantees. On macOS, it's only a hint to the scheduler. Wrong core assignments (e.g., pinning two hot threads to the same physical core) would hurt performance. Make it configurable with a sensible default.

---

## Change 3: Busy-Poll WebSocket with lws_service(ctx, 0)

**File:** `src/data/websocket_client.cpp` lines 175-183

**Problem:** Even after Phase 1's removal of the 10ms sleep, `lws_service(context_, 1)` still blocks for up to 1ms per call waiting for events. This adds up to 1ms of latency per poll cycle.

**After:** Use `lws_service(context_, 0)` for non-blocking polling in a tight loop:

```cpp
while (running_.load(std::memory_order_relaxed)) {
    lws_service(context_, 0);  // Non-blocking: return immediately
    flushTxQueue();

    // Optional: _mm_pause() to reduce power/thermal throttling
    // when no data was processed
    _mm_pause();
}
```

`lws_service(ctx, 0)` processes any pending events and returns immediately, never blocking. This gives the lowest possible latency for incoming data: the message is processed on the very next iteration after it arrives in the kernel socket buffer.

**Trade-off:** This pins one core at 100% CPU usage. This is standard for HFT -- dedicated cores are expected. For dev environments, keep the Phase 1 version (`lws_service(ctx, 1)`) behind a config flag:

```
# config.txt
WS_POLL_MODE=busy    # or "blocking" for dev
```

**Risk:** Low. The only concern is CPU power usage, which is irrelevant in production HFT.

---

## Change 4: Inline Pre-Trade Risk Checks

**Files:** `src/engine.cpp` lines 167-214, `src/execution/executor.cpp` lines 87-93

**Problem:** The current risk management runs on a separate thread that polls every 100ms. It checks `canPlaceOrder()` for both BUY and SELL, then sets `risk_breach_` atomically. The order engine reads `risk_breach_` to gate order placement. This means:
1. Risk breaches take up to 100ms to propagate to the order engine
2. The risk thread acquires 4 mutexes per cycle (`financial_mutex_`, `position_mutex_`, `operational_mutex_`, potentially `events_mutex_`) -- even though these checks are simple arithmetic

**Approach:** Split risk management into hot and cold paths:

**Hot path (inline in executor):** Simple limit checks that run on every order:
```cpp
bool OrderExecutor::check_risk_limits_inline(
    char side, double qty, double cached_pos, double max_pos) const {
    double new_pos = cached_pos + ((side == 'B') ? qty : -qty);
    return std::abs(new_pos) <= max_pos;
}
```

This is what `check_risk_limits` already does, but the function can be marked `inline` or `constexpr`-like -- it's pure arithmetic with no atomics or locks.

**Cold path (keep on risk thread):** Complex checks that run periodically:
- Financial limits (daily loss, drawdown) -- these depend on PnL which changes slowly
- Order rate limits -- these need time-window tracking
- Circuit breaker monitoring
- Risk event recording

The risk thread continues to run at 100ms intervals for these slower checks. The key change is that the hot path never calls into `RiskManager` at all -- it only checks local cached values.

**Risk:** Low. The hot-path risk check is already doing this (just position limits). The change is mainly about removing the cross-thread `canPlaceOrder` calls from the risk thread and making the inline check the sole gate on the trading path.

---

## Change 5: Eliminate Double Signal Generation

**File:** `src/engine.cpp` lines 126-164

**Problem:** The `order_engine_worker` generates signals from two sources in the same loop iteration:
1. When SPSC queue pops data (lines 131-139): runs strategy on the queued market data
2. When the timer fires (lines 141-153): runs strategy on the latest bid/ask from `market_data_feed_`

When both trigger in the same iteration, the strategy runs twice and two ladders are placed -- doubling the orders for that tick. This is a logic bug that also wastes execution time.

**After (with Change 1's merged model):** This problem disappears entirely because the strategy runs once per market data update, directly in the callback. No queue, no timer.

**If Change 1 is deferred:** Fix the current dual-path by removing the timer path entirely and relying solely on the SPSC queue:

```cpp
while (running_.load(std::memory_order_relaxed)) {
    bool did_work = false;

    HFTMarketData market_data{};
    while (market_data_queue_.pop(market_data)) {
        did_work = true;
        // Process all queued updates, only trade on the latest
    }

    if (did_work) {
        HFTSignal signal = strategy_->generate_signal(
            market_data.bid_price, market_data.ask_price,
            current_position_.load(std::memory_order_relaxed),
            order_size_.load(std::memory_order_relaxed));
        if (signal.place_bid || signal.place_ask) {
            executor_->place_order_ladder(signal);
        }
    }

    // Process fill responses
    HFTOrder response{};
    while (executor_->pop_response(response)) {
        executor_->process_order_response(response);
    }

    if (!did_work) {
        for (int i = 0; i < 32; ++i) _mm_pause();
    }
}
```

This drains all queued updates but only generates a signal on the most recent one (the last `market_data` after the inner while loop). This avoids stale signals while still reacting to every tick.

**Risk:** Low-medium. Removing the timer changes the trading behavior -- the bot only trades when new market data arrives. If the feed goes quiet, no orders are placed. This is correct for market-making (you don't want to trade on stale data).

---

## Change 6: Fix wsi_ Data Race in disconnect()

**File:** `src/data/websocket_client.cpp` lines 64-72

**Problem:** `disconnect()` sets `wsi_ = nullptr` from the calling thread (engine thread), while the WebSocket worker thread may be in `lws_service()` which uses `wsi_`. This is a data race -- `wsi_` is a raw pointer with no synchronization.

**Before:**
```cpp
void WebSocketClient::disconnect() {
    connected_ = false;
    std::cout << "WebSocket disconnecting..." << std::endl;

    if (wsi_) {
        lws_close_reason(wsi_, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
        wsi_ = nullptr;
    }
}
```

**After:** Don't touch `wsi_` from outside the worker thread. Instead, signal the worker to shut down gracefully:

```cpp
void WebSocketClient::disconnect() {
    connected_.store(false, std::memory_order_release);
    running_.store(false, std::memory_order_release);

    // Wake up the WS thread if it's in lws_service
    if (context_) {
        lws_cancel_service(context_);
    }
}
```

Let the worker thread's main loop handle the cleanup:
```cpp
// In workerLoop, after the while loop exits:
if (wsi_) {
    lws_close_reason(wsi_, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
    wsi_ = nullptr;
}
```

**Risk:** Low. This is a correctness fix, not an optimization. The current code has a potential use-after-free.

---

## Change 7: Replace std::endl with '\n' Everywhere

**Files:** All `.cpp` files that use `std::cout` or file streams

**Problem:** `std::endl` is equivalent to `'\n' << std::flush`. The `flush()` forces a `write()` syscall, adding ~1-5us per line. The bot has dozens of `std::endl` calls across logger, metrics, risk manager, and WebSocket client. During the metrics 10-second summary, there are ~9 consecutive flushes.

**Change:** Global find-and-replace `std::endl` with `'\n'` in all non-critical output paths. Keep `std::endl` (or explicit `flush()`) only for:
- Error messages that must be visible immediately before a crash
- The final line of shutdown output

**Files to update:**
- `src/core/logger.cpp` line 59, 63 (both `std::endl`)
- `src/metrics/metrics.cpp` lines 35, 56-64 (many `std::endl`)
- `src/risk/risk_manager.cpp` lines 33-35, 43, 235, 258
- `src/data/websocket_client.cpp` lines 44, 51, 55, 66, 106, 130, 149, 168, 178, 186, 195, 205, 210, 237-238, 245, 343
- `src/engine.cpp` lines 21, 58-61, 78, 86, 92, 106, 120, 168, 180, 188, 225
- `src/order/order_manager.cpp` lines 19, 28, 265-267

**Risk:** Very low. `'\n'` produces identical visible output. The stream flushes when its buffer fills (typically 4-8KB) or at program exit. If immediate visibility is needed for debugging, add explicit `std::cout.flush()` after critical log sections.

---

## Change 8: Make Logger Async

**Files:** `src/core/logger.cpp`, `include/core/logger.h`

**Problem:** Every `logger_->info()` call acquires `log_mutex_`, formats the message, writes to cout, writes to file, and flushes -- all synchronously. If any thread calls the logger on the hot path (or near it), it blocks for the duration of I/O.

**Approach:** Push log entries onto a lock-free queue and have a dedicated background thread drain them:

```cpp
struct LogEntry {
    LogLevel level;
    std::array<char, 256> message;  // Fixed-size, no heap allocation
    uint8_t message_len;
    std::chrono::system_clock::time_point timestamp;
};

// In Logger:
SPSCQueue<LogEntry, 4096> log_queue_;  // Lock-free queue
std::thread log_writer_thread_;

void Logger::log(LogLevel level, const std::string& message) {
    if (level < current_level_) return;

    LogEntry entry;
    entry.level = level;
    entry.timestamp = std::chrono::system_clock::now();
    entry.message_len = static_cast<uint8_t>(
        std::min(message.size(), size_t{255}));
    std::memcpy(entry.message.data(), message.data(), entry.message_len);

    log_queue_.push(entry);  // Non-blocking, lock-free
}

// Background writer thread:
void Logger::writerLoop() {
    LogEntry entry;
    while (running_) {
        if (log_queue_.pop(entry)) {
            // Format and write to file + console
            // This is the only thread that does I/O
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // Drain remaining entries on shutdown
    while (log_queue_.pop(entry)) { /* write */ }
}
```

**Caveat:** The SPSC queue only supports one producer. If multiple threads log, this needs an MPSC queue. A simple approach: use one SPSC queue per thread, and have the writer thread round-robin drain them. Alternatively, since logging from the hot path should be avoided anyway, use the SPSC for the hot thread and a mutex-based path for others.

**Risk:** Medium. Async logging means messages may not appear immediately before a crash. Add a `flush()` method that blocks until the queue is drained, and call it in the shutdown path.

---

## Verification

After applying all Phase 4 changes:

1. **Build:** Verify compilation on both macOS and Linux (the CPU pinning code is platform-specific).
2. **Functionality test:** Run the bot for 60s and verify identical trading behavior (same number of trades, similar PnL characteristics).
3. **Latency measurement:** Measure tick-to-trade latency by timestamping the market data arrival (in the WS callback) and the order ladder completion. With the merged model (Change 1), this should be measurable in a single thread with `rdtsc` or `high_resolution_clock`.
4. **CPU profiling:** Use `perf` (Linux) or Instruments (macOS) to verify the hot thread stays on its pinned core and isn't being migrated.
5. **Crash safety:** Verify the `wsi_` data race fix (Change 6) by running the bot and sending SIGINT during active trading. The shutdown should be clean with no segfaults.

---

## Complexity Summary

| Change | Effort | Risk | Impact |
|--------|--------|------|--------|
| 1. Merge hot path to single thread | High | High | Very High (~1-5 us) |
| 2. CPU pinning | Low-Med | Low-Med | Medium (jitter reduction) |
| 3. Busy-poll lws_service(0) | Low | Low | High (~0-1ms wakeup) |
| 4. Inline risk checks | Low | Low | Medium (~100ns/order) |
| 5. Eliminate double signal gen | Low-Med | Low-Med | Medium (correctness) |
| 6. Fix wsi_ data race | Low | Low | High (correctness) |
| 7. Replace endl with \n | Low | Very Low | Low (~1-5us/line) |
| 8. Async logger | Medium | Medium | Medium (~5-50us/log) |
