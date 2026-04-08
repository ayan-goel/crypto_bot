#include "engine.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/types.h"
#include "data/websocket_client.h"
#include "data/market_data.h"
#include "strategy/market_maker.h"
#include "execution/executor.h"
#include "order/order_manager.h"
#include "risk/risk_manager.h"
#include "metrics/metrics.h"
#include <iostream>
#include <cmath>

HFTEngine::HFTEngine() = default;
HFTEngine::~HFTEngine() { stop(); }

bool HFTEngine::initialize(const std::string& config_file) {
    Config& config = Config::getInstance();
    if (!config.loadFromFile(config_file)) {
        std::cerr << "Failed to load config: " << config_file << std::endl;
        return false;
    }

    logger_ = &Logger::getInstance();
    logger_->initialize("logs");
    logger_->info("HFT Engine initialization started");

    trading_symbol_ = config.getConfig("TRADING_SYMBOL", "ETH-USD");

    risk_manager_ = std::make_unique<RiskManager>();
    if (!risk_manager_->initialize(config_file)) {
        logger_->error("Failed to initialize risk manager");
        return false;
    }

    order_manager_ = std::make_unique<OrderManager>();
    if (!order_manager_->initialize()) {
        logger_->error("Failed to initialize order manager");
        return false;
    }

    websocket_client_ = std::make_unique<WebSocketClient>();
    websocket_client_->setApiCredentials(config.getCoinbaseApiKey(), config.getCoinbaseSecretKey());

    metrics_ = std::make_unique<MetricsCollector>(*order_manager_);
    market_data_feed_ = std::make_unique<MarketDataFeed>(*websocket_client_, metrics_->metrics());
    strategy_ = std::make_unique<MarketMakingStrategy>();
    executor_ = std::make_unique<OrderExecutor>(
        trading_symbol_, *order_manager_, metrics_->metrics(),
        current_position_, risk_breach_, max_position_);

    order_size_.store(config.getOrderSize());
    max_position_.store(config.getMaxInventory());
    order_engine_hz_ = config.getOrderEngineHz();

    logger_->info("HFT Engine initialized - config ready");
    std::cout << "HFT Engine Ready" << std::endl;
    std::cout << "   Symbol: " << trading_symbol_
              << " | Size: " << order_size_.load() << " ETH"
              << " | Max Pos: " << max_position_.load() << " ETH" << std::endl;

    return true;
}

void HFTEngine::start() {
    if (running_.load()) return;
    running_.store(true);

    std::string ws_url = Config::getInstance().getCoinbaseWsUrl();
    if (!websocket_client_->connect(ws_url)) {
        logger_->error("Failed to connect WebSocket for market data");
        running_.store(false);
        return;
    }

    websocket_client_->subscribeOrderBook(trading_symbol_, 10, 100);
    market_data_feed_->start(trading_symbol_, market_data_queue_);
    std::cout << trading_symbol_ << " market data connected" << std::endl;

    order_engine_thread_ = std::thread(&HFTEngine::order_engine_worker, this);
    risk_thread_ = std::thread(&HFTEngine::risk_management_worker, this);
    metrics_thread_ = std::thread(&HFTEngine::metrics_worker, this);

    logger_->info("HFT Engine started - All worker threads running");
    std::cout << "Trading Engine Active" << std::endl;
}

void HFTEngine::stop() {
    if (!running_.load()) return;

    std::cout << "Stopping HFT Engine..." << std::endl;
    logger_->info("HFT Engine shutdown initiated");
    running_.store(false);

    if (websocket_client_) websocket_client_->disconnect();

    if (order_engine_thread_.joinable()) order_engine_thread_.join();
    if (risk_thread_.joinable()) risk_thread_.join();
    if (metrics_thread_.joinable()) metrics_thread_.join();

    if (order_manager_) order_manager_->shutdown();
    metrics_->print_performance_stats();

    std::cout << "HFT Engine stopped" << std::endl;
    logger_->info("HFT Engine shutdown completed");
}

void HFTEngine::order_engine_worker() {
    std::cout << "Order engine worker started" << std::endl;
    logger_->info("Order engine worker started");

    auto last_order_time = std::chrono::high_resolution_clock::now();
    const auto target_interval = std::chrono::microseconds(1000000 / order_engine_hz_);

    while (running_.load(std::memory_order_relaxed)) {
        auto now = std::chrono::high_resolution_clock::now();
        bool did_work = false;

        HFTMarketData market_data{};
        if (market_data_queue_.pop(market_data)) {
            did_work = true;
            HFTSignal signal = strategy_->generate_signal(
                market_data.bid_price, market_data.ask_price,
                current_position_.load(), order_size_.load());
            if (signal.place_bid || signal.place_ask) {
                executor_->place_order_ladder(signal);
            }
        }

        if (now - last_order_time >= target_interval) {
            did_work = true;
            double bid = market_data_feed_->bid();
            double ask = market_data_feed_->ask();
            if (bid > 0 && ask > 0) {
                HFTSignal signal = strategy_->generate_signal(
                    bid, ask, current_position_.load(), order_size_.load());
                if (signal.place_bid || signal.place_ask) {
                    executor_->place_order_ladder(signal);
                }
            }
            last_order_time = now;
        }

        HFTOrder response{};
        while (executor_->pop_response(response)) {
            did_work = true;
            executor_->process_order_response(response);
        }

        if (!did_work) {
            std::this_thread::yield();
        }
    }
}

void HFTEngine::risk_management_worker() {
    std::cout << "Risk management worker started" << std::endl;
    logger_->info("Risk management worker started");

    double last_pnl = 0.0;

    while (running_.load()) {
        double pos = current_position_.load();
        metrics_->metrics().current_position.store(pos);
        risk_manager_->updatePosition(trading_symbol_, pos);

        if (risk_manager_->isCircuitBreakerActive()) {
            logger_->error("Circuit breaker is active - stopping trading");
            std::cout << "RISK BREACH: Circuit breaker active!" << std::endl;
            emergency_stop();
            break;
        }

        auto risk_status = risk_manager_->getCurrentRiskStatus();
        if (risk_status == RiskStatus::EMERGENCY) {
            logger_->error("Emergency risk status - stopping trading");
            std::cout << "RISK BREACH: Emergency risk status!" << std::endl;
            emergency_stop();
            break;
        }

        std::string rejection_reason;
        bool can_buy = risk_manager_->canPlaceOrder(trading_symbol_, "BUY",
            market_data_feed_->ask(), order_size_.load(), rejection_reason);
        bool can_sell = risk_manager_->canPlaceOrder(trading_symbol_, "SELL",
            market_data_feed_->bid(), order_size_.load(), rejection_reason);

        if (!can_buy && !can_sell) {
            logger_->warning("Risk limits preventing all trading: " + rejection_reason);
            risk_breach_.store(true);
        } else {
            risk_breach_.store(false);
        }

        double current_pnl = order_manager_->getCurrentPnL();
        double pnl_delta = current_pnl - last_pnl;
        if (std::abs(pnl_delta) > 0.001) {
            risk_manager_->updatePnL(pnl_delta);
            last_pnl = current_pnl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void HFTEngine::metrics_worker() {
    while (running_.load()) {
        metrics_->tick();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void HFTEngine::emergency_stop() {
    std::cout << "EMERGENCY STOP TRIGGERED!" << std::endl;
    logger_->error("Emergency stop triggered");
    risk_breach_.store(true);
    running_.store(false);
}
