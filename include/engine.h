#pragma once

#include "core/spsc_queue.h"
#include "data/market_data.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>

class WebSocketClient;
class RiskManager;
class OrderManager;
class MarketMakingStrategy;
class OrderExecutor;
class MetricsCollector;
class Logger;

class HFTEngine {
public:
    HFTEngine();
    ~HFTEngine();

    bool initialize(const std::string& config_file);
    void start();
    void stop();
    bool is_running() const { return running_.load(); }

private:
    std::unique_ptr<WebSocketClient> websocket_client_;
    std::unique_ptr<RiskManager> risk_manager_;
    std::unique_ptr<OrderManager> order_manager_;
    std::unique_ptr<MarketMakingStrategy> strategy_;
    std::unique_ptr<OrderExecutor> executor_;
    std::unique_ptr<MetricsCollector> metrics_;
    std::unique_ptr<MarketDataFeed> market_data_feed_;
    Logger* logger_ = nullptr;

    std::string trading_symbol_;

    std::atomic<bool> running_{false};
    std::thread market_data_thread_;
    std::thread order_engine_thread_;
    std::thread risk_thread_;
    std::thread metrics_thread_;

    SPSCQueue<HFTMarketData, 1024> market_data_queue_;

    std::atomic<double> order_size_{0.005};
    std::atomic<double> max_position_{0.1};
    std::atomic<double> current_position_{0.0};
    std::atomic<bool> risk_breach_{false};

    int order_engine_hz_ = 2000;

    void market_data_worker();
    void order_engine_worker();
    void risk_management_worker();
    void metrics_worker();
    void emergency_stop();
};
