#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <climits>

class OrderManager;

struct AtomicHFTMetrics {
    // Written by order engine / executor thread
    alignas(64) std::atomic<uint64_t> orders_placed{0};
    std::atomic<uint64_t> orders_filled{0};
    std::atomic<double> total_pnl{0.0};

    // Written by risk management thread
    alignas(64) std::atomic<double> current_position{0.0};

    // Written by WebSocket / market data thread
    alignas(64) std::atomic<uint64_t> market_data_updates{0};
    std::atomic<uint64_t> websocket_latency_ns{0};

    // Written by order engine thread (latency tracking)
    alignas(64) std::atomic<uint64_t> avg_order_latency_ns{0};
    std::atomic<uint64_t> min_order_latency_ns{UINT64_MAX};
    std::atomic<uint64_t> max_order_latency_ns{0};

    // Written by metrics thread
    alignas(64) std::atomic<uint64_t> orders_per_second{0};
};

class MetricsCollector {
public:
    explicit MetricsCollector(OrderManager& order_manager);

    AtomicHFTMetrics& metrics() { return metrics_; }
    const AtomicHFTMetrics& metrics() const { return metrics_; }

    void tick();
    void print_performance_stats() const;

private:
    AtomicHFTMetrics metrics_;
    OrderManager& order_manager_;
    std::chrono::high_resolution_clock::time_point engine_start_time_;

    uint64_t last_rate_orders_ = 0;
    uint64_t last_rate_time_ms_ = 0;

    uint64_t last_orders_filled_ = 0;
    double last_pnl_ = 0.0;
    std::chrono::steady_clock::time_point last_summary_;
    std::chrono::steady_clock::time_point last_print_;

    void update_trading_rate();
};
