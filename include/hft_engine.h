#pragma once

#include <atomic>
#include <thread>
#include <vector>
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <limits>

// Include Order struct definition
#include "strategy.h"

// Forward declarations
class WebSocketClient;
class RiskManager;
class OrderManager;
class Logger;

// Lock-free circular buffer for order queue
template<typename T, size_t Size>
class LockFreeQueue {
public:
    bool push(const T& item) {
        const auto current_tail = tail_.load();
        const auto next_tail = increment(current_tail);
        if (next_tail != head_.load()) {
            buffer_[current_tail] = item;
            tail_.store(next_tail);
            return true;
        }
        return false; // Queue full
    }
    
    bool pop(T& item) {
        const auto current_head = head_.load();
        if (current_head == tail_.load()) {
            return false; // Queue empty
        }
        item = buffer_[current_head];
        head_.store(increment(current_head));
        return true;
    }
    
    size_t size() const {
        return (tail_.load() - head_.load()) % Size;
    }
    
private:
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
    std::array<T, Size> buffer_;
    
    size_t increment(size_t idx) const {
        return (idx + 1) % Size;
    }
};

// High-performance order structure
struct HFTOrder {
    uint64_t order_id;
    uint64_t client_order_id;
    char symbol[16];
    char side; // 'B' or 'S'
    double price;
    double quantity;
    double filled_quantity;
    char status; // 'N'ew, 'F'illed, 'C'anceled
    std::chrono::high_resolution_clock::time_point timestamp;
    std::chrono::high_resolution_clock::time_point order_sent_time;
    std::chrono::high_resolution_clock::time_point fill_time;
    uint32_t priority; // For order ladder management
};

// Market data structure optimized for speed
struct HFTMarketData {
    char symbol[16];
    double bid_price;
    double ask_price;
    double bid_quantity;
    double ask_quantity;
    std::chrono::high_resolution_clock::time_point timestamp;
    uint64_t sequence_number;
};

// Trading signal for continuous market making
struct HFTSignal {
    bool place_bid;
    bool place_ask;
    bool cancel_orders;
    double bid_price;
    double ask_price;
    double bid_quantity;
    double ask_quantity;
    uint32_t num_levels; // Number of price levels to quote
};

// Performance metrics (non-atomic for returning)
struct HFTMetrics {
    uint64_t orders_placed{0};
    uint64_t orders_canceled{0};
    uint64_t orders_filled{0};
    uint64_t market_data_updates{0};
    double total_pnl{0.0};
    double current_position{0.0};
    
    // Latency metrics (nanoseconds)
    uint64_t avg_order_latency_ns{0};
    uint64_t min_order_latency_ns{UINT64_MAX};
    uint64_t max_order_latency_ns{0};
    uint64_t websocket_latency_ns{0};
    
    // Trading rate
    uint64_t orders_per_second{0};
    uint64_t last_rate_update{0};
};

// Internal atomic metrics for thread safety
struct AtomicHFTMetrics {
    std::atomic<uint64_t> orders_placed{0};
    std::atomic<uint64_t> orders_canceled{0};
    std::atomic<uint64_t> orders_filled{0};
    std::atomic<uint64_t> market_data_updates{0};
    std::atomic<double> total_pnl{0.0};
    std::atomic<double> current_position{0.0};
    
    // Latency metrics (nanoseconds)
    std::atomic<uint64_t> avg_order_latency_ns{0};
    std::atomic<uint64_t> min_order_latency_ns{UINT64_MAX};
    std::atomic<uint64_t> max_order_latency_ns{0};
    std::atomic<uint64_t> websocket_latency_ns{0};
    
    // Trading rate
    std::atomic<uint64_t> orders_per_second{0};
    std::atomic<uint64_t> last_rate_update{0};
};

class HFTEngine {
public:
    HFTEngine();
    ~HFTEngine();
    
    // Engine lifecycle
    bool initialize(const std::string& config_file);
    void start();
    void stop();
    bool is_running() const { return running_.load(); }
    
    // Real-time metrics
    HFTMetrics get_metrics() const;
    void print_performance_stats() const;
    
    // Configuration
    void set_target_spread_bps(double bps) { target_spread_bps_.store(bps); }
    void set_order_size(double size) { order_size_.store(size); }
    void set_max_position(double pos) { max_position_.store(pos); }
    void set_orders_per_second(uint32_t rate) { target_order_rate_.store(rate); }

private:
    // Integrated components
    std::unique_ptr<WebSocketClient> websocket_client_;
    std::unique_ptr<RiskManager> risk_manager_;
    std::unique_ptr<OrderManager> order_manager_;
    Logger* logger_;
    
    // Trading configuration
    double initial_capital_{50.0};
    std::atomic<uint64_t> sequence_counter_{0};
    
    // Core threading
    std::atomic<bool> running_{false};
    std::thread market_data_thread_;
    std::thread order_engine_thread_;
    std::thread risk_thread_;
    std::thread metrics_thread_;
    
    // Lock-free queues for inter-thread communication
    LockFreeQueue<HFTMarketData, 1024> market_data_queue_;
    LockFreeQueue<HFTOrder, 2048> outbound_order_queue_;
    LockFreeQueue<HFTOrder, 2048> inbound_order_queue_;
    
    // Current market state (lock-free)
    std::atomic<double> current_bid_{0.0};
    std::atomic<double> current_ask_{0.0};
    std::atomic<double> current_spread_bps_{0.0};
    std::atomic<uint64_t> last_market_update_{0};
    
    // Trading parameters (atomic for lock-free access)
    std::atomic<double> target_spread_bps_{0.1};
    std::atomic<double> order_size_{0.005};
    std::atomic<double> max_position_{0.1};
    std::atomic<uint32_t> target_order_rate_{100}; // orders per second
    
    // Risk limits
    std::atomic<double> current_position_{0.0};
    std::atomic<double> daily_pnl_{0.0};
    std::atomic<bool> risk_breach_{false};
    
    // Performance tracking
    AtomicHFTMetrics metrics_;
    std::chrono::high_resolution_clock::time_point engine_start_time_;
    
    // Order management
    std::atomic<uint64_t> next_order_id_{1};
    std::array<HFTOrder, 100> active_orders_; // Fixed-size for performance
    std::atomic<size_t> active_order_count_{0};
    
    // Thread functions
    void market_data_worker();
    void order_engine_worker();
    void risk_management_worker();
    void metrics_worker();
    
    // Core trading logic
    HFTSignal generate_signal(const HFTMarketData& market_data);
    void place_order_ladder(const HFTSignal& signal);
    void cancel_stale_orders();
    void update_risk_metrics();
    
    // Order execution
    bool send_order(const HFTOrder& order);
    void process_order_response(const HFTOrder& response);
    uint64_t generate_order_id();
    
    // Performance monitoring
    void update_latency_metrics(uint64_t latency_ns);
    void update_trading_rate();
    
    // Risk management
    bool check_risk_limits(const HFTOrder& order);
    void emergency_stop();
    
    // Session management
    void generate_session_summary();
}; 