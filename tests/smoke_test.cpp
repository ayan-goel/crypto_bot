#include "core/config.h"
#include "core/types.h"
#include "core/spsc_queue.h"
#include "data/market_data.h"
#include "strategy/market_maker.h"
#include "execution/executor.h"
#include "order/order_manager.h"
#include "risk/risk_manager.h"
#include "metrics/metrics.h"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <cassert>

int main() {
    std::cout << "=== HFT Bot Smoke Test ===" << std::endl;

    Config& config = Config::getInstance();
    if (!config.loadFromFile("config.txt")) {
        std::cerr << "Failed to load config.txt" << std::endl;
        return 1;
    }

    RiskManager risk_manager;
    risk_manager.initialize("config.txt");

    OrderManager order_manager;
    order_manager.initialize();

    MetricsCollector metrics(order_manager);
    std::atomic<double> current_position{0.0};
    std::atomic<bool> risk_breach{false};
    std::atomic<double> max_position{config.getMaxInventory()};

    OrderExecutor executor("ETH-USD", order_manager, metrics.metrics(),
                           current_position, risk_breach, max_position);

    MarketMakingStrategy strategy;

    double order_size = config.getOrderSize();

    std::cout << "\n--- Strategy Signal Generation ---" << std::endl;
    double sim_bid = 1850.50;
    double sim_ask = 1850.60;

    HFTSignal signal = strategy.generate_signal(sim_bid, sim_ask, 0.0, order_size);
    std::cout << "Market: " << sim_bid << " / " << sim_ask
              << " (spread: " << std::fixed << std::setprecision(2)
              << (sim_ask - sim_bid) << ")" << std::endl;
    std::cout << "Signal: bid=" << signal.place_bid << " ask=" << signal.place_ask
              << " levels=" << signal.num_levels << std::endl;
    std::cout << "  Bid price: " << std::setprecision(4) << signal.bid_price
              << " qty: " << signal.bid_quantity << std::endl;
    std::cout << "  Ask price: " << signal.ask_price
              << " qty: " << signal.ask_quantity << std::endl;

    assert(signal.place_bid && "Strategy should place bids");
    assert(signal.place_ask && "Strategy should place asks");
    assert(signal.bid_price < sim_bid && "Bid should be below market bid");
    assert(signal.ask_price > sim_ask && "Ask should be above market ask");
    assert(signal.num_levels > 0 && "Should have ladder levels");

    std::cout << "\n--- Order Execution (100 cycles) ---" << std::endl;
    int total_fills = 0;
    for (int i = 0; i < 100; ++i) {
        double jitter = (i % 3 - 1) * 0.01;
        HFTSignal sig = strategy.generate_signal(
            sim_bid + jitter, sim_ask + jitter,
            current_position.load(), order_size);

        if (sig.place_bid || sig.place_ask) {
            executor.place_order_ladder(sig);
        }

        HFTOrder response{};
        while (executor.pop_response(response)) {
            executor.process_order_response(response);
            total_fills++;
        }
    }

    double pos = current_position.load();
    double pnl = order_manager.getCurrentPnL();
    uint64_t trades = order_manager.getTotalTrades();

    std::cout << "Fills: " << total_fills << std::endl;
    std::cout << "Trades (OrderManager): " << trades << std::endl;
    std::cout << "Position: " << std::setprecision(6) << pos << " ETH" << std::endl;
    std::cout << "PnL: $" << std::setprecision(6) << pnl << std::endl;
    std::cout << "Orders placed (metrics): " << metrics.metrics().orders_placed.load() << std::endl;
    std::cout << "Orders filled (metrics): " << metrics.metrics().orders_filled.load() << std::endl;

    assert(total_fills > 0 && "Should have some fills from simulation");
    assert(trades == static_cast<uint64_t>(total_fills) && "OrderManager trades should match fills");
    assert(std::abs(pos) <= max_position.load() + order_size &&
           "Position should stay near max inventory");

    std::cout << "\n--- Risk Manager Position Tracking ---" << std::endl;
    risk_manager.updatePosition("ETH-USD", pos);
    risk_manager.updatePnL(pnl);

    std::string rejection;
    bool can_buy = risk_manager.canPlaceOrder("ETH-USD", "BUY", sim_ask, order_size, rejection);
    bool can_sell = risk_manager.canPlaceOrder("ETH-USD", "SELL", sim_bid, order_size, rejection);
    std::cout << "Can BUY:  " << (can_buy ? "yes" : "no") << std::endl;
    std::cout << "Can SELL: " << (can_sell ? "yes" : "no") << std::endl;
    if (!rejection.empty()) {
        std::cout << "Rejection: " << rejection << std::endl;
    }

    std::cout << "\n--- Inventory Skew Test ---" << std::endl;
    HFTSignal skew_sig = strategy.generate_signal(sim_bid, sim_ask, 0.012, order_size);
    std::cout << "At +0.012 pos -> bid_qty=" << std::setprecision(6) << skew_sig.bid_quantity
              << " ask_qty=" << skew_sig.ask_quantity << std::endl;
    assert(skew_sig.ask_quantity > skew_sig.bid_quantity &&
           "Long position should skew to sell more");

    HFTSignal skew_sig2 = strategy.generate_signal(sim_bid, sim_ask, -0.012, order_size);
    std::cout << "At -0.012 pos -> bid_qty=" << skew_sig2.bid_quantity
              << " ask_qty=" << skew_sig2.ask_quantity << std::endl;
    assert(skew_sig2.bid_quantity > skew_sig2.ask_quantity &&
           "Short position should skew to buy more");

    std::cout << "\n--- SPSC Queue Test ---" << std::endl;
    SPSCQueue<HFTMarketData, 16> queue;
    HFTMarketData md{};
    set_symbol(md.symbol, "ETH-USD");
    md.bid_price = 1850.50;
    md.ask_price = 1850.60;

    for (int i = 0; i < 20; ++i) {
        md.sequence_number = i;
        queue.push(md);
    }
    int popped = 0;
    HFTMarketData out{};
    while (queue.pop(out)) popped++;
    std::cout << "Pushed 20 into size-16 queue, popped " << popped << std::endl;
    assert(popped == 15 && "Queue capacity is Size-1 = 15 for ring buffer");

    std::cout << "\n--- PnL Correctness Test ---" << std::endl;
    OrderManager pnl_test;
    pnl_test.initialize();

    pnl_test.placeOrder("ETH-USD", Side::BUY, 1850.00, 0.01);
    pnl_test.placeOrder("ETH-USD", Side::SELL, 1851.00, 0.01);
    double expected_pnl = (1851.00 - 1850.00) * 0.01;
    double actual_pnl = pnl_test.getCurrentPnL();
    std::cout << "Buy@1850, Sell@1851 x 0.01 -> PnL: $" << std::setprecision(6)
              << actual_pnl << " (expected: $" << expected_pnl << ")" << std::endl;
    assert(std::abs(actual_pnl - expected_pnl) < 1e-9 && "PnL should be (1851-1850)*0.01 = $0.01");

    pnl_test.placeOrder("ETH-USD", Side::SELL, 1849.00, 0.005);
    pnl_test.placeOrder("ETH-USD", Side::SELL, 1848.00, 0.005);
    pnl_test.placeOrder("ETH-USD", Side::BUY, 1847.00, 0.01);
    double short_pnl = pnl_test.getCurrentPnL();
    double expected_short_pnl = expected_pnl
        + (1849.00 - 1847.00) * 0.005
        + (1848.00 - 1847.00) * 0.005;
    std::cout << "Short@1849(0.005)+1848(0.005), cover@1847 -> Cumulative PnL: $"
              << short_pnl << " (expected: $" << expected_short_pnl << ")" << std::endl;
    assert(std::abs(short_pnl - expected_short_pnl) < 1e-9 && "Short PnL should be correct");

    double final_pos = pnl_test.getCurrentPosition();
    std::cout << "Final position: " << final_pos << " (expected: 0.0)" << std::endl;
    assert(std::abs(final_pos) < 1e-9 && "Position should be flat");

    std::cout << "\n--- Latency Metrics ---" << std::endl;
    std::cout << "Avg order latency: "
              << metrics.metrics().avg_order_latency_ns.load() / 1000.0 << " us" << std::endl;
    std::cout << "Min order latency: "
              << metrics.metrics().min_order_latency_ns.load() / 1000.0 << " us" << std::endl;
    std::cout << "Max order latency: "
              << metrics.metrics().max_order_latency_ns.load() / 1000.0 << " us" << std::endl;

    metrics.tick();
    metrics.print_performance_stats();

    order_manager.shutdown();

    std::cout << "\n=== ALL TESTS PASSED ===" << std::endl;
    return 0;
}
