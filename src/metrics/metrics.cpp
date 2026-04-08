#include "metrics/metrics.h"
#include "order/order_manager.h"
#include <iostream>
#include <iomanip>
#include <algorithm>

MetricsCollector::MetricsCollector(OrderManager& order_manager)
    : order_manager_(order_manager)
{
    engine_start_time_ = std::chrono::high_resolution_clock::now();
    last_summary_ = std::chrono::steady_clock::now();
    last_print_ = last_summary_;
}

void MetricsCollector::tick() {
    auto now = std::chrono::steady_clock::now();

    update_trading_rate();

    if (now - last_summary_ >= std::chrono::seconds(5)) {
        uint64_t current_total_trades = order_manager_.getTotalTrades();
        double current_pnl = order_manager_.getCurrentPnL();
        double current_position = order_manager_.getCurrentPosition();

        uint64_t trades_delta = current_total_trades - last_orders_filled_;
        double pnl_delta = current_pnl - last_pnl_;

        double avg_latency_ms = metrics_.avg_order_latency_ns.load(std::memory_order_relaxed) / 1000000.0;

        std::cout << "5s: " << trades_delta << " trades"
                  << " | PnL: $" << std::fixed << std::setprecision(6) << pnl_delta
                  << " | Pos: " << std::setprecision(6) << current_position << " ETH"
                  << " | Order: " << std::setprecision(3) << avg_latency_ms << "ms"
                  << " | Total: " << current_total_trades
                  << " | Cumulative PnL: $" << std::setprecision(6) << current_pnl << std::endl;

        last_orders_filled_ = current_total_trades;
        last_pnl_ = current_pnl;
        last_summary_ = now;
    }

    if (now - last_print_ >= std::chrono::seconds(10)) {
        print_performance_stats();
        last_print_ = now;
    }
}

void MetricsCollector::print_performance_stats() const {
    auto runtime = std::chrono::high_resolution_clock::now() - engine_start_time_;
    auto runtime_seconds = std::chrono::duration_cast<std::chrono::seconds>(runtime).count();

    uint64_t total_trades = order_manager_.getTotalTrades();
    double current_pnl = order_manager_.getCurrentPnL();
    double current_position = order_manager_.getCurrentPosition();

    std::cout << "\nPERFORMANCE (10s Update)" << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << "Runtime: " << runtime_seconds << "s" << std::endl;
    std::cout << "Total Trades: " << total_trades << std::endl;
    std::cout << "Position: " << std::fixed << std::setprecision(6) << current_position << " ETH" << std::endl;
    std::cout << "PnL: $" << std::setprecision(6) << current_pnl << std::endl;
    std::cout << "Avg Trades/sec: " << std::setprecision(2)
              << (total_trades / std::max(1LL, static_cast<long long>(runtime_seconds))) << std::endl;
    std::cout << "=========================================\n" << std::endl;
}

void MetricsCollector::update_trading_rate() {
    auto now = std::chrono::high_resolution_clock::now();
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());

    uint64_t current_orders = metrics_.orders_placed.load(std::memory_order_relaxed);
    uint64_t orders_delta = current_orders - last_rate_orders_;
    uint64_t time_delta = now_ms - last_rate_time_ms_;

    if (time_delta >= 1000) {
        uint64_t rate = (orders_delta * 1000) / time_delta;
        metrics_.orders_per_second.store(rate, std::memory_order_relaxed);
        last_rate_orders_ = current_orders;
        last_rate_time_ms_ = now_ms;
    }
}
