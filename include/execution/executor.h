#pragma once

#include "core/spsc_queue.h"
#include <array>
#include <atomic>
#include <chrono>
#include <random>
#include <string>
#include <cstdint>

struct HFTSignal;
struct AtomicHFTMetrics;
class OrderManager;

struct HFTOrder {
    uint64_t order_id = 0;
    uint64_t client_order_id = 0;
    std::array<char, 16> symbol{};
    char side = 0;
    double price = 0.0;
    double quantity = 0.0;
    double filled_quantity = 0.0;
    char status = 0;
    std::chrono::high_resolution_clock::time_point timestamp;
    std::chrono::high_resolution_clock::time_point order_sent_time;
    std::chrono::high_resolution_clock::time_point fill_time;
    uint32_t priority = 0;
};

class OrderExecutor {
public:
    OrderExecutor(const std::string& trading_symbol,
                  OrderManager& order_manager,
                  AtomicHFTMetrics& metrics,
                  std::atomic<double>& current_position,
                  std::atomic<bool>& risk_breach,
                  std::atomic<double>& max_position);

    void place_order_ladder(const HFTSignal& signal);
    void process_order_response(const HFTOrder& response);
    bool pop_response(HFTOrder& response);
    bool check_risk_limits(const HFTOrder& order, double current_pos, double max_pos) const;

private:
    std::string trading_symbol_;
    OrderManager& order_manager_;
    AtomicHFTMetrics& metrics_;
    std::atomic<double>& current_position_;
    std::atomic<bool>& risk_breach_;
    std::atomic<double>& max_position_;

    SPSCQueue<HFTOrder, 2048> inbound_order_queue_;
    std::atomic<uint64_t> next_order_id_{1};

    std::mt19937 rng_;
    std::uniform_real_distribution<> fill_distribution_{0.0, 1.0};

    double tick_size_;
    static constexpr double MIN_ORDER_QTY = 0.001;

    HFTOrder build_order(char side, double price, double quantity, uint32_t level);
    bool send_order(HFTOrder& order);
    uint64_t generate_order_id();
    void update_latency_metrics(uint64_t latency_ns);
};
