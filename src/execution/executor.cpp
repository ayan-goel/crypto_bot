#include "execution/executor.h"
#include "strategy/market_maker.h"
#include "order/order_manager.h"
#include "metrics/metrics.h"
#include "core/config.h"
#include "core/types.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <thread>

OrderExecutor::OrderExecutor(const std::string& trading_symbol,
                             OrderManager& order_manager,
                             AtomicHFTMetrics& metrics,
                             std::atomic<double>& current_position,
                             std::atomic<bool>& risk_breach,
                             std::atomic<double>& max_position)
    : trading_symbol_(trading_symbol)
    , order_manager_(order_manager)
    , metrics_(metrics)
    , current_position_(current_position)
    , risk_breach_(risk_breach)
    , max_position_(max_position)
{
    std::random_device rd;
    rng_ = std::mt19937(rd());
    tick_size_ = Config::getInstance().getTickSize();
}

void OrderExecutor::place_order_ladder(const HFTSignal& signal) {
    auto start_time = std::chrono::high_resolution_clock::now();

    for (uint32_t level = 0; level < signal.num_levels; ++level) {
        double level_offset = level * tick_size_ * 0.1;
        double level_size_factor = 1.0 - level * 0.1;

        if (signal.place_bid && !risk_breach_.load()) {
            HFTOrder bid = build_order('B',
                signal.bid_price - level_offset,
                signal.bid_quantity * level_size_factor,
                level);
            if (check_risk_limits(bid) && send_order(bid)) {
                metrics_.orders_placed.fetch_add(1);
            }
        }

        if (signal.place_ask && !risk_breach_.load()) {
            HFTOrder ask = build_order('S',
                signal.ask_price + level_offset,
                signal.ask_quantity * level_size_factor,
                level);
            if (check_risk_limits(ask) && send_order(ask)) {
                metrics_.orders_placed.fetch_add(1);
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto latency_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
    update_latency_metrics(latency_ns);
}

void OrderExecutor::process_order_response(const HFTOrder& response) {
    if (response.status != 'F') return;

    metrics_.orders_filled.fetch_add(1);

    double position_change = (response.side == 'B') ? response.filled_quantity : -response.filled_quantity;
    double old_pos = current_position_.load();
    while (!current_position_.compare_exchange_weak(old_pos, old_pos + position_change)) {}

    Side side = (response.side == 'B') ? Side::BUY : Side::SELL;
    order_manager_.placeOrder(response.symbol.data(), side, response.price, response.filled_quantity);

    metrics_.total_pnl.store(order_manager_.getCurrentPnL());
}

bool OrderExecutor::pop_response(HFTOrder& response) {
    return inbound_order_queue_.pop(response);
}

bool OrderExecutor::check_risk_limits(const HFTOrder& order) const {
    if (risk_breach_.load()) return false;

    double position_change = (order.side == 'B') ? order.quantity : -order.quantity;
    double new_position = current_position_.load() + position_change;

    return std::abs(new_position) <= max_position_.load();
}

HFTOrder OrderExecutor::build_order(char side, double price, double quantity, uint32_t level) {
    HFTOrder order{};
    order.order_id = generate_order_id();
    order.client_order_id = order.order_id;
    set_symbol(order.symbol, trading_symbol_);
    order.side = side;
    order.price = price;
    order.quantity = quantity;
    order.filled_quantity = 0.0;
    order.status = 'N';
    order.timestamp = std::chrono::high_resolution_clock::now();
    order.order_sent_time = order.timestamp;
    order.priority = level;
    return order;
}

bool OrderExecutor::send_order(HFTOrder& order) {
    std::this_thread::sleep_for(std::chrono::microseconds(10));

    double current_pos = current_position_.load();
    double base_fill_probability = 0.3;

    if ((order.side == 'S' && current_pos > 0.01) ||
        (order.side == 'B' && current_pos < -0.01)) {
        base_fill_probability *= 1.8;
    }

    if ((order.side == 'B' && current_pos > 0.01) ||
        (order.side == 'S' && current_pos < -0.01)) {
        base_fill_probability *= 0.4;
    }

    base_fill_probability = std::min(base_fill_probability, 0.65);

    if (fill_distribution_(rng_) < base_fill_probability) {
        HFTOrder filled_order = order;
        filled_order.status = 'F';
        filled_order.filled_quantity = filled_order.quantity;
        filled_order.fill_time = std::chrono::high_resolution_clock::now();
        inbound_order_queue_.push(filled_order);
    }

    return true;
}

uint64_t OrderExecutor::generate_order_id() {
    return next_order_id_.fetch_add(1);
}

void OrderExecutor::update_latency_metrics(uint64_t latency_ns) {
    uint64_t current_min = metrics_.min_order_latency_ns.load();
    while (latency_ns < current_min && !metrics_.min_order_latency_ns.compare_exchange_weak(current_min, latency_ns)) {}

    uint64_t current_max = metrics_.max_order_latency_ns.load();
    while (latency_ns > current_max && !metrics_.max_order_latency_ns.compare_exchange_weak(current_max, latency_ns)) {}

    uint64_t current_avg = metrics_.avg_order_latency_ns.load();
    uint64_t new_avg = (current_avg + latency_ns) / 2;
    metrics_.avg_order_latency_ns.store(new_avg);
}
