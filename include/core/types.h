#pragma once

#include <string>
#include <chrono>
#include <array>
#include <cstring>
#include <algorithm>

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

static constexpr int kIdleSpinCount = 32;
static constexpr int kIdleSpinFallbackThreshold = 1000;

// Branch prediction hints for hot paths. Use HFT_LIKELY when a condition is
// true >99% of the time (normal data flow); HFT_UNLIKELY for error/rare paths.
#define HFT_LIKELY(x)   __builtin_expect(!!(x), 1)
#define HFT_UNLIKELY(x) __builtin_expect(!!(x), 0)

enum class Side { BUY, SELL };

enum class OrderStatus { PENDING, NEW, FILLED, PARTIALLY_FILLED, CANCELED, REJECTED };

enum class OrderType { LIMIT, MARKET };

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

inline const char* to_string(Side s) {
    return s == Side::BUY ? "BUY" : "SELL";
}

inline const char* to_string(OrderStatus s) {
    switch (s) {
        case OrderStatus::PENDING:          return "PENDING";
        case OrderStatus::NEW:              return "NEW";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::CANCELED:         return "CANCELED";
        case OrderStatus::REJECTED:         return "REJECTED";
    }
    return "UNKNOWN";
}

inline void set_symbol(std::array<char, 16>& dest, const std::string& src) {
    size_t len = std::min(src.size(), dest.size() - 1);
    std::memcpy(dest.data(), src.data(), len);
    dest[len] = '\0';
}
