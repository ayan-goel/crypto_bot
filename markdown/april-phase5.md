# Phase 5: Systems-Level and Advanced Optimizations

**Goal:** Squeeze the last microseconds out of the pipeline with systems-level techniques -- memory management, compiler optimizations, kernel-level I/O, and architectural patterns borrowed from top-tier HFT firms. These changes require deeper systems knowledge and may have platform-specific limitations.

**Target improvement:** ~1-5 us reduction in tail latency (p99/p99.9). Significant reduction in latency jitter from OS-level sources (page faults, TLB misses, syscalls).

**Dependencies:** Phases 1-4. These optimizations have diminishing returns if the higher-level bottlenecks (JSON parsing, mutexes, thread hops) haven't been addressed first.

---

## Change 1: mlockall at Startup

**File:** `src/main.cpp`

**Problem:** Virtual memory pages are loaded on-demand by the OS. The first access to a previously untouched page triggers a page fault (~1-10us), which involves a kernel trap, page table walk, and potentially disk I/O (for swapped pages). Even minor page faults (no disk I/O) cost ~1us. During trading, a single page fault can blow your p99 latency by 10x.

**Implementation:**
```cpp
#include <sys/mman.h>

int main(int argc, char* argv[]) {
    // Lock all current and future memory pages into physical RAM.
    // Prevents page faults during trading.
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::cerr << "WARNING: mlockall failed (run as root or set "
                  << "CAP_IPC_LOCK): " << strerror(errno) << '\n';
        // Non-fatal: continue without memory locking
    }

    // ... rest of main ...
}
```

**Requirements:**
- On Linux: requires `CAP_IPC_LOCK` capability or root. Set via: `sudo setcap cap_ipc_lock+ep ./build/crypto_hft_engine`
- On macOS: `mlockall` is available but limited -- it locks current pages but `MCL_FUTURE` may not work as expected. Use `mlock()` on specific allocations instead.
- Memory limit: `mlockall` locks ALL memory (including shared libraries, stack, heap). Ensure the system has enough physical RAM. For a trading bot using ~50-100MB, this is trivial.

**Risk:** Low. If `mlockall` fails (insufficient privileges), the bot continues normally. Add a warning log but don't abort.

---

## Change 2: Huge Pages for Critical Data Structures

**Files:** `include/core/spsc_queue.h`, `include/data/market_data.h`, new `include/core/huge_page_alloc.h`

**Problem:** Standard 4KB pages require frequent TLB (Translation Lookaside Buffer) lookups. The TLB has a limited number of entries (~64-1536 on modern x86). When the working set exceeds what the TLB can map, TLB misses cause page table walks (~10-100ns each). The SPSC queue buffer (1024 * sizeof(HFTMarketData) = ~64KB) and the order book span multiple pages.

**Implementation:** Allocate critical structures on 2MB huge pages:

```cpp
// include/core/huge_page_alloc.h
#pragma once
#include <cstddef>
#include <sys/mman.h>

inline void* alloc_huge_page(size_t size) {
    // Round up to 2MB boundary
    constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;
    size_t aligned_size = (size + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);

#ifdef __linux__
    void* p = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) {
        // Fallback to regular pages
        p = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
#else
    // macOS: no huge page support via mmap flags.
    // Use VM_FLAGS_SUPERPAGE_SIZE_2MB via mach_vm_allocate for Apple Silicon,
    // or fall back to regular allocation.
    void* p = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif

    if (p != MAP_FAILED) {
        mlock(p, aligned_size);  // Also lock into physical RAM
    }
    return (p == MAP_FAILED) ? nullptr : p;
}

inline void free_huge_page(void* p, size_t size) {
    if (p) {
        constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;
        size_t aligned_size = (size + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
        munmap(p, aligned_size);
    }
}
```

**Usage:** Allocate the SPSC queue buffer on huge pages:
```cpp
// Custom SPSC queue variant that uses huge-page-backed storage
// instead of std::array on the stack
template<typename T, size_t Size>
class HugePageSPSCQueue {
    // buffer_ allocated via alloc_huge_page in constructor
};
```

**Linux setup:**
```bash
# Pre-allocate huge pages at boot or runtime:
echo 64 > /proc/sys/vm/nr_hugepages   # 64 * 2MB = 128MB
```

**Risk:** Medium. Huge pages require system configuration (Linux) and may not be available on all deployments. The fallback to regular pages ensures the bot still works. On macOS, the benefit is limited.

---

## Change 3: Object Pool Allocator

**Files:** New `include/core/object_pool.h`, `src/execution/executor.cpp`, `src/order/order_manager.cpp`

**Problem:** Even after Phases 2-3, there may be residual heap allocations in non-obvious places (string formatting in risk events, log entries, or future code additions). An object pool provides O(1) allocation with zero heap interaction.

**Implementation:**
```cpp
// include/core/object_pool.h
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

template <typename T, size_t N>
class ObjectPool {
public:
    ObjectPool() {
        for (size_t i = 0; i < N; ++i) {
            freelist_[i] = i;
        }
    }

    T* allocate() {
        if (free_count_ == 0) return nullptr;
        size_t idx = freelist_[--free_count_];
        return reinterpret_cast<T*>(&storage_[idx]);
    }

    void deallocate(T* p) {
        size_t idx = static_cast<size_t>(
            reinterpret_cast<std::aligned_storage_t<sizeof(T), alignof(T)>*>(p)
            - storage_.data());
        freelist_[free_count_++] = idx;
    }

    template<typename... Args>
    T* construct(Args&&... args) {
        T* p = allocate();
        if (p) new (p) T(std::forward<Args>(args)...);
        return p;
    }

    void destroy(T* p) {
        if (p) {
            p->~T();
            deallocate(p);
        }
    }

private:
    std::array<std::aligned_storage_t<sizeof(T), alignof(T)>, N> storage_;
    std::array<size_t, N> freelist_;
    size_t free_count_ = N;
};
```

**Usage:** Pre-allocate pools at startup for types that are frequently created/destroyed:
```cpp
// In OrderExecutor or OrderManager:
ObjectPool<HFTOrder, 4096> order_pool_;
```

**Risk:** Low. The pool is additive -- existing code can gradually migrate to use it. The fixed capacity `N` must be sized appropriately (overflow returns nullptr, which must be handled).

---

## Change 4: std::pmr::monotonic_buffer_resource for Per-Tick Scratch

**Files:** `src/data/market_data_feed.cpp`, `src/execution/executor.cpp`

**Problem:** Some operations need temporary allocations within a single tick (e.g., building a vector of price levels during a snapshot, or accumulating fill responses). These are currently stack-allocated or use heap-allocated STL containers.

**Approach:** Use C++17 polymorphic memory resources to allocate from a pre-allocated buffer that's reset after each tick:

```cpp
#include <memory_resource>

// Pre-allocated 64KB buffer for per-tick work
alignas(64) static char tick_buffer[65536];
static std::pmr::monotonic_buffer_resource tick_resource(
    tick_buffer, sizeof(tick_buffer));

// Usage in a per-tick function:
void process_tick() {
    tick_resource.release();  // Reset to beginning (O(1), no deallocation)

    std::pmr::vector<double> prices(&tick_resource);
    prices.reserve(50);  // Allocates from tick_buffer, not heap

    // ... use prices ...
    // At function exit, prices destructor runs but no heap free occurs
}
```

**Risk:** Low. `std::pmr` is standard C++17. The monotonic buffer never actually frees memory -- it just resets the pointer, making "allocation" a simple pointer bump (~2ns). The buffer must be large enough for worst-case per-tick usage.

---

## Change 5: CRTP-Based Strategy Dispatch

**Files:** `include/strategy/market_maker.h`, `include/engine.h`, `src/engine.cpp`

**Problem:** The strategy is held as `std::unique_ptr<MarketMakingStrategy>`. Calling `strategy_->generate_signal()` is a virtual dispatch through the unique_ptr's indirection (~2-5ns). More critically, the compiler cannot inline `generate_signal` through the pointer, missing optimization opportunities.

**Before:**
```cpp
std::unique_ptr<MarketMakingStrategy> strategy_;
// ...
HFTSignal signal = strategy_->generate_signal(bid, ask, pos, size);
```

**After (CRTP approach):** Template the engine on the strategy type:

```cpp
// Base template
template <typename Derived>
class StrategyBase {
public:
    HFTSignal generate_signal(double bid, double ask,
                              double pos, double size) const {
        return static_cast<const Derived*>(this)->generate_signal_impl(
            bid, ask, pos, size);
    }
};

// Concrete strategy
class MarketMakingStrategy : public StrategyBase<MarketMakingStrategy> {
public:
    MarketMakingStrategy();
    HFTSignal generate_signal_impl(double bid, double ask,
                                    double pos, double size) const;
private:
    // ... same fields ...
};

// Engine templated on strategy:
template <typename Strategy>
class HFTEngine {
    Strategy strategy_;  // Direct member, no pointer indirection
    // ...
};

// In main.cpp:
HFTEngine<MarketMakingStrategy> engine;
```

The compiler can now fully inline `generate_signal_impl` into the caller, eliminating the indirect call and enabling further optimizations (constant propagation, loop unrolling, etc.).

**Trade-off:** The engine becomes a template, which means:
- Header-heavy implementation (or explicit template instantiation in a .cpp file)
- Cannot swap strategies at runtime (must recompile)
- More complex to add new strategies (each needs a template instantiation)

For an HFT system that runs a single strategy, this is the right trade-off. If runtime strategy selection is needed, keep the virtual dispatch for the selection path and use CRTP within the selected strategy's hot loop.

**Risk:** Medium. Significant refactoring of the engine class. All methods that use `strategy_` must be updated. Test thoroughly.

---

## Change 6: Pre-Computed Order Templates

**Files:** `src/execution/executor.cpp`, `include/execution/executor.h`

**Problem:** `build_order()` constructs a fresh `HFTOrder` struct on every call, initializing ~15 fields. Most fields are invariant across orders (symbol, side template, status, priority pattern). Only price and quantity change per order.

**Approach:** Pre-compute order templates at initialization, then patch only the variable fields at trade time:

```cpp
// In OrderExecutor constructor:
HFTOrder bid_template_{};
HFTOrder ask_template_{};

OrderExecutor::OrderExecutor(...) {
    // Build templates once
    set_symbol(bid_template_.symbol, trading_symbol_);
    bid_template_.side = 'B';
    bid_template_.status = 'N';

    set_symbol(ask_template_.symbol, trading_symbol_);
    ask_template_.side = 'S';
    ask_template_.status = 'N';
}

// Hot path: just patch price/qty/id/timestamp
HFTOrder OrderExecutor::build_bid(double price, double qty, uint32_t level) {
    HFTOrder order = bid_template_;  // memcpy of pre-initialized struct
    order.order_id = generate_order_id();
    order.client_order_id = order.order_id;
    order.price = price;
    order.quantity = qty;
    order.filled_quantity = 0.0;
    order.priority = level;
    auto now = std::chrono::high_resolution_clock::now();
    order.timestamp = now;
    order.order_sent_time = now;
    return order;
}
```

The template copy is a fast memcpy of a ~104-byte struct (fits in 2 cache lines). Compared to field-by-field initialization, this is slightly faster and more importantly, it's more maintainable -- new fields added to `HFTOrder` automatically get the template's default.

**Risk:** Low. This is a minor optimization but establishes the pattern for future order serialization (when sending to a real exchange, the wire-format template can be pre-built and only price/qty bytes patched).

---

## Change 7: rdtsc for Hot-Path Timestamps

**Files:** New `include/core/timestamp.h`, `src/execution/executor.cpp`, `src/data/market_data_feed.cpp`

**Problem:** `std::chrono::high_resolution_clock::now()` resolves to `clock_gettime(CLOCK_MONOTONIC)` on Linux, which is a vDSO call (~20-25ns). It's called multiple times per tick: in `build_order()`, `place_order_ladder()` (for latency measurement), `market_data_feed` (for timing). Each call adds ~20ns.

**Approach:** Use the CPU's Time Stamp Counter (TSC) for sub-nanosecond timestamps on the hot path. Calibrate the TSC frequency at startup.

```cpp
// include/core/timestamp.h
#pragma once
#include <cstdint>

#ifdef __x86_64__
#include <x86intrin.h>
inline uint64_t rdtsc() {
    return __rdtsc();  // ~3ns, no syscall
}
#elif defined(__aarch64__)
inline uint64_t rdtsc() {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
#endif

// Calibrated conversion (set once at startup)
struct TSCCalibration {
    uint64_t tsc_start;
    uint64_t ns_start;
    double tsc_per_ns;  // Calibrated ratio

    void calibrate();  // Run at startup, takes ~100ms
    uint64_t tsc_to_ns(uint64_t tsc) const {
        return static_cast<uint64_t>((tsc - tsc_start) / tsc_per_ns) + ns_start;
    }
};
```

**Calibration:** Compare TSC delta to `clock_gettime` delta over a 100ms sleep at startup. This gives the TSC frequency with ~0.1% accuracy.

**Usage:** Replace `high_resolution_clock::now()` on the hot path with `rdtsc()` for latency measurements. Keep `high_resolution_clock` for timestamps that need to be serialized or compared across machines.

**Risk:** Medium. TSC frequency may change with CPU frequency scaling (must disable via `cpupower frequency-set -g performance` on Linux). On Apple Silicon (M-series), the equivalent `cntvct_el0` counter runs at a fixed frequency and is reliable. On VMs, TSC may not be available or reliable.

---

## Change 8: __builtin_prefetch for Order Book Access

**File:** `src/data/market_data_feed.cpp`

**Problem:** When the order engine reads the BBO (best bid/ask) from the flat order book array (after Phase 2), the first access may cause an L1 cache miss if the data hasn't been accessed recently (~3-5ns for L1 hit vs ~10-15ns for L2/L3).

**Implementation:**
```cpp
// Before accessing the book, prefetch the first cache line:
__builtin_prefetch(&bid_levels_[0], 0, 3);  // Read, high temporal locality
__builtin_prefetch(&ask_levels_[0], 0, 3);

// ... do other work (strategy computation uses other data) ...

// Now access the book -- it's already in L1 cache:
double best_bid = bid_levels_[0].price;
double best_ask = ask_levels_[0].price;
```

The prefetch is only useful if there's other work between the prefetch and the access (at least ~50-100ns). In the merged-thread model (Phase 4), the strategy computation happens between parsing the update and reading the BBO, providing a natural window for prefetch.

**Risk:** Very low. If the data is already in cache, the prefetch is a no-op. If it's not, you save ~5-10ns. The compiler may insert prefetches automatically in some cases; this makes it explicit.

---

## Change 9: Explore io_uring for Kernel-Level I/O (Linux Only)

**File:** `src/data/websocket_client.cpp` (potential replacement of libwebsockets)

**Problem:** libwebsockets uses `poll()` internally, which is a syscall. Even `lws_service(ctx, 0)` enters the kernel to check for events. On Linux, `io_uring` can process I/O without syscalls in steady state (the kernel polls a shared ring buffer).

**This is an exploration item, not a concrete implementation plan.** The scope is:

1. Evaluate replacing libwebsockets with a custom WebSocket client built on `io_uring`:
   - Use `liburing` for the low-level ring buffer management
   - Implement WebSocket framing manually (it's a simple protocol on top of TCP)
   - Use `IORING_SETUP_SQPOLL` for kernel-side polling (zero syscalls in steady state)

2. Alternative: Use Boost.Beast (header-only WebSocket library) with Boost.Asio, configured to use `io_uring` backend (Asio supports io_uring as of Boost 1.78):
   ```cpp
   // Asio with io_uring backend
   boost::asio::io_context ioc;
   // ... set up WebSocket stream with Beast ...
   // Busy-poll:
   while (running) {
       ioc.poll();  // Uses io_uring internally on Linux 5.1+
   }
   ```

**Trade-off:**
- Custom io_uring client: Maximum control, minimum overhead, but significant development effort (~2-4 weeks)
- Boost.Beast + Asio: Well-tested, feature-complete, lower effort, but heavier dependency and slightly higher overhead than raw io_uring

**Risk:** High effort, platform-specific (Linux-only). For crypto exchanges accessed over the internet (not co-located), the network RTT (1-50ms) dwarfs the ~1-5us kernel overhead, making this optimization less impactful than co-located scenarios. Prioritize only if targeting ultra-low-latency co-located setups.

---

## Change 10: Explore Replacing libwebsockets with a Leaner Client

**Problem beyond io_uring:** libwebsockets is a general-purpose library with features the bot doesn't need (HTTP server, multiple protocol support, per-connection user data). Its callback architecture imposes constraints (single callback function with a switch statement, global state via `client_instance` pointer).

**Options:**

### Option A: Boost.Beast (Recommended for near-term)
- Header-only, well-maintained, C++11 compatible
- Direct control over the I/O loop (can busy-poll via `ioc.poll()`)
- Supports TLS via Boost.Asio SSL
- No global state issues
- Effort: ~1 week to port

### Option B: uWebSockets
- Very high performance, widely benchmarked
- C-style API with C++ wrappers
- Effort: ~3-5 days to port

### Option C: Raw socket + manual WebSocket framing
- Maximum control, zero abstraction overhead
- Must implement: TLS handshake (via OpenSSL), HTTP upgrade, frame masking, ping/pong, fragmentation
- Effort: ~2-3 weeks
- Only justified if co-locating with the exchange

**Risk:** Medium-high effort depending on the chosen option. Beast is the safest incremental improvement. Raw sockets are only justified for co-located deployments where every microsecond counts.

---

## Verification

After applying Phase 5 changes:

1. **mlockall:** Verify via `/proc/self/status` that `VmLck` shows the expected locked memory.
2. **Huge pages:** Check `/proc/meminfo` for `HugePages_Free` decreasing after startup.
3. **Object pool:** Run under Valgrind/ASan to verify no memory leaks from the pool.
4. **Latency profiling:** Use `perf stat` to measure:
   - Page faults (should be near-zero after mlockall)
   - TLB misses (should decrease with huge pages)
   - Branch mispredictions (should decrease with likely/unlikely from Phase 1)
   - IPC (instructions per cycle, should increase with better cache behavior)
5. **End-to-end latency:** Measure tick-to-trade with rdtsc timestamps. Target: <5us internal processing for the full pipeline (market data parse -> strategy -> order build).

---

## Complexity Summary

| Change | Effort | Risk | Impact |
|--------|--------|------|--------|
| 1. mlockall | Low | Low | High (eliminates page faults) |
| 2. Huge pages | Medium | Medium | Medium (TLB miss reduction) |
| 3. Object pool | Medium | Low | Medium (zero hot-path malloc) |
| 4. pmr scratch allocator | Low | Low | Low-Medium (per-tick allocs) |
| 5. CRTP strategy dispatch | Medium | Medium | Low-Medium (~2-5ns/call) |
| 6. Pre-computed order templates | Low | Low | Low (~5-10ns/order) |
| 7. rdtsc timestamps | Medium | Medium | Medium (~20ns/call saved) |
| 8. Prefetch hints | Low | Very Low | Low (~5-10ns/access) |
| 9. io_uring exploration | Very High | High | Medium (platform-specific) |
| 10. WS library replacement | High | Medium-High | Medium (cleaner arch) |
