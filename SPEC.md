# SPEC: Phase 1 — Foundation and Quick Wins

## Objective

Apply 10 targeted, low-risk latency fixes to the existing HFT engine with no architectural changes.
Each fix is surgical: touches exactly one concern, leaves surrounding logic unchanged.

**Expected outcome:** ~10–30 µs off hot-path order latency; elimination of the 10–60 ms WebSocket poll floor.

**Status:** All 10 changes are pending. No Phase 1 changes have been applied yet.

---

## Build & Run Commands

```bash
# Release build
cmake --build build --config Release

# Smoke test (no API credentials needed)
cd /path/to/crypto_bot
./build/smoke_test

# Full run (requires config.txt with valid credentials)
./build/crypto_hft_engine
```

The smoke test is the primary verification gate. It must pass — with all assertions — after every change.

---

## Files Modified

| File | Changes |
|------|---------|
| `include/core/spsc_queue.h` | Changes 1, 2 |
| `include/core/types.h` | Change 10 (add branch-hint macros) |
| `include/engine.h` | Changes 4, 9 |
| `include/metrics/metrics.h` | Change 9 |
| `src/data/websocket_client.cpp` | Changes 3, 7 |
| `src/data/market_data_feed.cpp` | Change 5 |
| `src/engine.cpp` | Changes 4, 6, 8 |
| `src/execution/executor.cpp` | Changes 5, 6 |
| `src/metrics/metrics.cpp` | Change 5 |

No new files are needed. No CMakeLists changes. No new dependencies.

---

## Changes

### Change 1 — Fix SPSC Queue False Sharing
**File:** `include/core/spsc_queue.h`

`head_` (written by consumer) and `tail_` (written by producer) currently share a cache line, causing MESI-protocol bouncing between cores on every push/pop (~50–100 ns/op).

**Acceptance criteria:**
- `head_`, `tail_`, and `buffer_` are each aligned to 64-byte cache line boundaries.
- Padding between `head_` and `tail_` prevents sharing.
- Existing push/pop logic is unchanged.

---

### Change 2 — Replace Modulo with Bitmask in SPSC Queue
**File:** `include/core/spsc_queue.h`

`(idx + 1) % Size` generates a division instruction (~20–40 cycles) on every push and pop.

**Acceptance criteria:**
- `increment()` uses `(idx + 1) & (Size - 1)` instead of `% Size`.
- A `static_assert((Size & (Size - 1)) == 0, "SPSCQueue size must be a power of two")` is added.
- Existing queue instantiations are already power-of-2 (`1024`, `2048`, `16`). No size changes needed.
- Smoke test SPSC section still passes (`popped == 15` for a size-16 queue).

---

### Change 3 — Remove 10 ms Sleep from WebSocket Worker Loop
**File:** `src/data/websocket_client.cpp`

`workerLoop()` unconditionally sleeps 10 ms after each `lws_service()` call, adding a 10–60 ms latency floor to every market data message.

**Acceptance criteria:**
- `std::this_thread::sleep_for(std::chrono::milliseconds(10))` is removed entirely.
- `lws_service` timeout is reduced from `50` to `1` ms.
- The `std::endl` in the error log is replaced with `'\n'` (avoids flush overhead).
- No other logic in `workerLoop()` is changed.

---

### Change 4 — Remove the Dead `market_data_worker` Thread
**Files:** `src/engine.cpp`, `include/engine.h`

`market_data_worker()` calls `market_data_feed_->start()` then busy-sleeps at 100 ms. The actual data processing happens on the WebSocket callback thread. This thread does nothing useful.

**Acceptance criteria:**
- `market_data_feed_->start()` is called from `HFTEngine::start()` directly, before launching threads.
- `market_data_worker()` method is removed from `engine.cpp`.
- `market_data_worker()` declaration is removed from `engine.h`.
- `market_data_thread_` member is removed from `engine.h`.
- The `stop()` method no longer joins `market_data_thread_`.
- The three remaining threads (order engine, risk, metrics) are unaffected.

---

### Change 5 — Switch Metrics Atomics to `memory_order_relaxed`
**Files:** `src/data/market_data_feed.cpp`, `src/execution/executor.cpp`, `src/metrics/metrics.cpp`, `src/engine.cpp`, `src/data/websocket_client.cpp`

All metrics counter stores/loads use the default `memory_order_seq_cst`, generating full `MFENCE` instructions (~10 ns each). Metrics consumers tolerate stale reads; sequential consistency is unnecessary overhead.

**Acceptance criteria:**
All of the following use `std::memory_order_relaxed`:
- `metrics_.websocket_latency_ns.store(...)` — `market_data_feed.cpp`
- `current_bid_.store(...)`, `current_ask_.store(...)`, `current_spread_bps_.store(...)`, `last_market_update_.store(...)` — `market_data_feed.cpp`
- `metrics_.market_data_updates.fetch_add(1)` — `market_data_feed.cpp`
- `metrics_.orders_placed.fetch_add(1)`, `metrics_.orders_filled.fetch_add(1)` — `executor.cpp`
- `metrics_.total_pnl.store(...)` — `executor.cpp`
- All corresponding `.load()` calls in `metrics.cpp` (`update_trading_rate`, `tick`, `print_performance_stats`)
- `running_.load()` in hot loops: `engine.cpp` order engine loop, `websocket_client.cpp` worker loop.

**Note:** The correctness-critical atomics (`current_position_` CAS loop, `risk_breach_` flag) are **not** changed in this step — they are handled in Change 6.

---

### Change 6 — Cache Atomic Values Locally in Hot Loops
**Files:** `src/execution/executor.cpp`, `src/engine.cpp`

`place_order_ladder()` calls `risk_breach_.load()` twice per ladder level (10 loads for 5 levels). `check_risk_limits()` calls `current_position_.load()` and `max_position_.load()` inside the loop. These values are stable within a single ladder execution.

**Acceptance criteria:**
- At the top of `place_order_ladder()`, capture:
  ```cpp
  const bool breached = risk_breach_.load(std::memory_order_relaxed);
  ```
  Use `breached` (not `risk_breach_.load()`) in the loop body.
- `check_risk_limits()` accepts pre-loaded position values as parameters, or the cached values are passed in. The function no longer loads these atomics internally.
- In `order_engine_worker()` (`engine.cpp`), `order_size_.load()` is cached once per iteration rather than called multiple times.

---

### Change 7 — Remove `string::find("error")` Scan on Every Message
**File:** `src/data/websocket_client.cpp`

`handleMessage()` calls `message.find("error")` on every raw message string before parsing JSON. This is O(n) on every incoming L2 update.

**Acceptance criteria:**
- The `message.find("error")` string scan is removed.
- Error detection checks `json_msg["type"] == "error"` on the already-parsed JSON object instead.
- This check happens **after** the successful parse path, not before.
- The error log uses `std::cerr` and `'\n'` (not `std::endl`).
- The catch block for JSON parse exceptions is unchanged.

---

### Change 8 — Replace `yield()` with `_mm_pause()` Tiered Spin
**File:** `src/engine.cpp`

`std::this_thread::yield()` in the idle path of `order_engine_worker()` is a `sched_yield()` syscall, causing 1–15 µs re-scheduling delay before the thread can process the next market data message.

**Acceptance criteria:**
- `std::this_thread::yield()` is replaced with a tiered idle strategy:
  - If `idle_count < 1000`: spin 32 × `_mm_pause()` (x86) or a `__asm volatile("yield")` (ARM/Apple Silicon).
  - If `idle_count >= 1000`: fall back to `std::this_thread::yield()` (dev-machine-friendly).
  - `idle_count` resets to 0 when work is found.
- The platform guard (`#ifdef __x86_64__` / `#elif defined(__aarch64__)`) is in `include/core/types.h` or a new `include/core/cpu_hints.h` header.
- The `_mm_pause()` path requires `#include <immintrin.h>` on x86. On ARM, use `__asm volatile("yield")`.

---

### Change 9 — Cache-Line Align Cross-Thread Shared Atomics
**Files:** `include/engine.h`, `include/metrics/metrics.h`

`current_position_` (written by order engine, read by risk thread) and `risk_breach_` (written by risk thread, read by order engine) are adjacent in `engine.h`, sharing a cache line. Every write to one invalidates the other in the remote core's cache.

**Acceptance criteria:**
In `engine.h`:
- `current_position_` is `alignas(64)`.
- `risk_breach_` is `alignas(64)`.
- `running_` is `alignas(64)`.
- `order_size_` and `max_position_` may share a cache line (they are rarely written).

In `metrics.h` (`AtomicHFTMetrics`):
- Fields written by different threads are on separate cache lines. Specifically, `orders_placed`, `orders_filled`, and `total_pnl` (written by executor/order engine) are separated from `current_position` (written by risk thread).
- Use `alignas(64)` on the first field of each "writer group", or add explicit padding.

---

### Change 10 — Add `likely`/`unlikely` Branch Hints
**File:** `include/core/types.h`

Error-handling branches on the hot path are taken <0.1% of the time, but the compiler has no way to know this, leading to suboptimal branch layout.

**Acceptance criteria:**
- Two macros are added to `include/core/types.h`:
  ```cpp
  #define HFT_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define HFT_UNLIKELY(x) __builtin_expect(!!(x), 0)
  ```
- Applied at the following call sites:
  - `engine.cpp` order engine loop: `if (HFT_LIKELY(market_data_queue_.pop(market_data)))`
  - `executor.cpp:66`: `if (HFT_UNLIKELY(response.status != 'F')) return;`
  - `executor.cpp:72`: `if (HFT_UNLIKELY(!result.success)) return;`
  - `executor.cpp:88`: `if (HFT_UNLIKELY(risk_breach_.load())) return false;` — but in Change 6 this becomes `if (HFT_UNLIKELY(breached)) return false;`
  - `market_data_feed.cpp:31`: the guard check `if (HFT_UNLIKELY(!message.contains(...)))`
  - `market_data_feed.cpp:88`: `if (HFT_UNLIKELY(best_bid >= best_ask)) continue;`
  - `websocket_client.cpp` callback null check: `if (HFT_UNLIKELY(!client_instance || ...))`

---

## Code Style

Follow the project's `CLAUDE.md` guide. Key rules for this phase:

- `snake_case` for variables and functions; `PascalCase` for types.
- No new magic numbers — name any constants added (e.g., `constexpr int kIdleSpinCount = 32`).
- `alignas(64)` is the only structural change needed; do not reorder or rename unrelated fields.
- `#ifdef` guards for platform-specific intrinsics (`__x86_64__`, `__aarch64__`).
- Do not add comments that restate the code. If a cache-line alignment needs explanation, one short comment is fine.
- No new files unless Change 8 puts the CPU hint inline in `types.h` (preferred) vs a new header.

---

## Testing Strategy

**Primary gate:** `./build/smoke_test` must pass all `assert()` checks after every change.

**Key assertions to watch:**
- SPSC queue capacity test: `assert(popped == 15)` — verifies size-16 ring buffer capacity is unchanged after Changes 1 & 2.
- All strategy signal, fill, PnL, and risk assertions must continue to pass.

**Verification after all 10 changes:**
1. `cmake --build build` succeeds with no new warnings.
2. `./build/smoke_test` passes all assertions.
3. (Optional, requires credentials) Start the bot for 30 s; verify WebSocket connects, order engine processes signals, session summary writes to `logs/session_summary.log`.
4. (Optional) Compare `avg_order_latency_ns` before and after — expect measurable improvement.

**No new test code is required for this phase.** The smoke test already exercises the full pipeline.

---

## Boundaries

### Always do
- Run `./build/smoke_test` after every change, before moving to the next.
- Keep each change isolated — do not bundle unrelated fixes.
- Preserve the public API of `SPSCQueue` (push/pop signatures unchanged).
- Keep error paths functional — relaxed ordering on metrics is safe; do not apply relaxed ordering to `current_position_` CAS or `risk_breach_` store in the risk thread.

### Ask first
- Any change to `CMakeLists.txt` or build configuration.
- Any new external dependency.
- Changing the config file format or adding config keys.
- Touching `RiskManager`, `OrderManager`, or `MarketMakingStrategy` logic.

### Never do
- Apply Phase 2 changes (simdjson, flat order book, callback API change) in this phase.
- Change `lws_service` timeout to 0 (busy-polls the kernel; use 1 ms as specified).
- Apply `memory_order_relaxed` to `current_position_` CAS loop or `risk_breach_` in the risk management worker — these require correct ordering.
- Remove the JSON parse exception handler in `handleMessage`.
- Break the smoke test's `assert(popped == 15)` invariant.
