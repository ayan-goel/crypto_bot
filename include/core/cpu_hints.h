#pragma once

// CPU spin-wait hint: yields the current logical core without leaving it.
// ~5 ns per call on x86 (PAUSE instruction); prevents memory order violations
// in tight spin loops and reduces power consumption.
#if defined(__x86_64__) || defined(_M_X64)
#  include <immintrin.h>
#  define HFT_CPU_RELAX() _mm_pause()
#elif defined(__aarch64__) || defined(__arm64__)
#  define HFT_CPU_RELAX() __asm__ volatile("yield" ::: "memory")
#else
#  include <thread>
#  define HFT_CPU_RELAX() std::this_thread::yield()
#endif

// Spin-wait tuning constants for the order engine idle path.
static constexpr int kIdleSpinCount = 32;
static constexpr int kIdleSpinFallbackThreshold = 1000;

// Branch prediction hints for hot paths.
// Use HFT_LIKELY when a condition is true >99% of the time (normal data flow).
// Use HFT_UNLIKELY for error/rare paths (<1% of the time).
#define HFT_LIKELY(x)   __builtin_expect(!!(x), 1)
#define HFT_UNLIKELY(x) __builtin_expect(!!(x), 0)
