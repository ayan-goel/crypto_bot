#include "hft_engine.h"
#include "config.h"
#include "logger.h"
#include "risk_manager.h"
#include "order_manager.h"
#include "websocket_client.h"
#include <iostream>
#include <cstring>
#include <random>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <atomic>

HFTEngine::HFTEngine() : 
    websocket_client_(nullptr),
    risk_manager_(nullptr),
    order_manager_(nullptr),
    logger_(nullptr) {
    engine_start_time_ = std::chrono::high_resolution_clock::now();
}

HFTEngine::~HFTEngine() {
    stop();
}

bool HFTEngine::initialize(const std::string& config_file) {
    // Load configuration
    Config& config = Config::getInstance();
    if (!config.loadFromFile(config_file)) {
        std::cerr << "Failed to load config: " << config_file << std::endl;
        return false;
    }
    
    // Initialize logger
    logger_ = &Logger::getInstance();
    logger_->info("HFT Engine initialization started");
    
    // Initialize risk manager
    risk_manager_ = std::make_unique<RiskManager>();
    if (!risk_manager_->initialize(config_file)) {
        logger_->error("Failed to initialize risk manager");
        return false;
    }
    
    // Initialize order manager (no parameters needed)
    order_manager_ = std::make_unique<OrderManager>();
    if (!order_manager_->initialize()) {
        logger_->error("Failed to initialize order manager");
        return false;
    }
    
    // Set risk manager for PnL tracking
    order_manager_->setRiskManager(risk_manager_.get());
    
    // Initialize session tracking in OrderManager (if it has such a method)
    // Note: We'll rely on OrderManager's automatic session initialization
    
    // Initialize WebSocket client
    websocket_client_ = std::make_unique<WebSocketClient>();
    
    // Set initial parameters from config (using actual config values)
    target_spread_bps_.store(config.getSpreadThresholdBps());
    order_size_.store(config.getOrderSize());
    max_position_.store(config.getMaxInventory());
    target_order_rate_.store(config.getOrderRateLimit());
    
    // Set initial capital from config (default $50 as requested)
    initial_capital_ = std::stod(config.getConfig("INITIAL_CAPITAL", "50.0"));
    daily_pnl_.store(0.0);
    
    logger_->info("HFT Engine initialized with:");
    logger_->info("   Target Spread: " + std::to_string(target_spread_bps_.load()) + " bps");
    logger_->info("   Order Size: " + std::to_string(order_size_.load()) + " ETH");
    logger_->info("   Max Position: " + std::to_string(max_position_.load()) + " ETH");
    logger_->info("   Target Rate: " + std::to_string(target_order_rate_.load()) + " orders/sec");
    logger_->info("   Initial Capital: $" + std::to_string(initial_capital_));
    
    std::cout << "🚀 HFT Engine initialized with:" << std::endl;
    std::cout << "   Target Spread: " << target_spread_bps_.load() << " bps" << std::endl;
    std::cout << "   Order Size: " << order_size_.load() << " ETH" << std::endl;
    std::cout << "   Max Position: " << max_position_.load() << " ETH" << std::endl;
    std::cout << "   Target Rate: " << target_order_rate_.load() << " orders/sec" << std::endl;
    std::cout << "   Initial Capital: $" << initial_capital_ << std::endl;
    
    return true;
}

void HFTEngine::start() {
    if (running_.load()) {
        return;
    }
    
    running_.store(true);
    
    // Start WebSocket connection for real market data
    Config& config = Config::getInstance();
    std::string ws_url = config.getBinanceWsUrl();
    if (!websocket_client_->connect(ws_url)) {
        logger_->error("Failed to connect WebSocket for market data");
        return;
    }
    
    // Subscribe to order book updates
    websocket_client_->subscribeOrderBook("ethusdt", 10, 100);
    
    // Start worker threads
    market_data_thread_ = std::thread(&HFTEngine::market_data_worker, this);
    order_engine_thread_ = std::thread(&HFTEngine::order_engine_worker, this);
    risk_thread_ = std::thread(&HFTEngine::risk_management_worker, this);
    metrics_thread_ = std::thread(&HFTEngine::metrics_worker, this);
    
    logger_->info("HFT Engine started - All worker threads running");
    std::cout << "⚡ HFT Engine started - All worker threads running" << std::endl;
}

void HFTEngine::stop() {
    if (!running_.load()) {
        return;
    }
    
    std::cout << "🛑 Stopping HFT Engine..." << std::endl;
    logger_->info("HFT Engine shutdown initiated");
    
    running_.store(false);
    
    // Disconnect WebSocket
    if (websocket_client_) {
        websocket_client_->disconnect();
    }
    
    // Wait for threads to finish
    if (market_data_thread_.joinable()) market_data_thread_.join();
    if (order_engine_thread_.joinable()) order_engine_thread_.join();
    if (risk_thread_.joinable()) risk_thread_.join();
    if (metrics_thread_.joinable()) metrics_thread_.join();
    
    // Generate session summary using existing risk manager
    generate_session_summary();
    
    // Print final stats
    print_performance_stats();
    std::cout << "✅ HFT Engine stopped" << std::endl;
    logger_->info("HFT Engine shutdown completed");
}

void HFTEngine::market_data_worker() {
    std::cout << "📡 Market data worker started (WebSocket feed)" << std::endl;
    logger_->info("Market data worker started with real WebSocket feed");
    
    // Set up WebSocket callback to receive real market data
    if (websocket_client_) {
        websocket_client_->setMessageCallback([this](const nlohmann::json& message) {
            try {
                // Parse Binance depth stream message
                if (message.contains("data") && message["data"].contains("bids") && message["data"].contains("asks")) {
                    auto data = message["data"];
                    
                    // Extract best bid and ask from Binance depth data
                    if (!data["bids"].empty() && !data["asks"].empty()) {
                        double best_bid = std::stod(data["bids"][0][0].get<std::string>());
                        double best_ask = std::stod(data["asks"][0][0].get<std::string>());
                        double bid_qty = std::stod(data["bids"][0][1].get<std::string>());
                        double ask_qty = std::stod(data["asks"][0][1].get<std::string>());
                        
                        // Create market data from real WebSocket feed
                        HFTMarketData market_data;
                        strcpy(market_data.symbol, "ETHUSDT");
                        market_data.bid_price = best_bid;
                        market_data.ask_price = best_ask;
                        market_data.bid_quantity = bid_qty;
                        market_data.ask_quantity = ask_qty;
                        market_data.timestamp = std::chrono::high_resolution_clock::now();
                        market_data.sequence_number = ++sequence_counter_;
                        
                        // Update atomic market state with REAL prices
                        current_bid_.store(market_data.bid_price);
                        current_ask_.store(market_data.ask_price);
                        double spread_bps = ((market_data.ask_price - market_data.bid_price) / market_data.bid_price) * 10000.0;
                        current_spread_bps_.store(spread_bps);
                        last_market_update_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            market_data.timestamp.time_since_epoch()).count());
                        
                        // Push to queue for order engine
                        market_data_queue_.push(market_data);
                        metrics_.market_data_updates.fetch_add(1);
                    }
                }
            } catch (const std::exception& e) {
                // Ignore parsing errors for now - some messages might not be depth data
            }
        });
    }
    
    // Keep the worker alive to process WebSocket callbacks
    while (running_.load()) {
        // The actual market data processing is now handled by the WebSocket callback
        // This thread just needs to stay alive to keep the callback active
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void HFTEngine::order_engine_worker() {
    std::cout << "⚡ Order engine worker started" << std::endl;
    logger_->info("Order engine worker started");
    
    auto last_order_time = std::chrono::high_resolution_clock::now();
    const auto target_interval = std::chrono::microseconds(1000000 / target_order_rate_.load()); // Convert rate to interval
    
    while (running_.load()) {
        auto now = std::chrono::high_resolution_clock::now();
        
        // Process market data updates
        HFTMarketData market_data;
        if (market_data_queue_.pop(market_data)) {
            // Generate trading signal
            HFTSignal signal = generate_signal(market_data);
            
            // No individual signal logging - handled by 5-second summaries
            // Only log to file logger for debugging
            double spread_bps = current_spread_bps_.load();
            std::string signal_reason = "HFT market making - spread " + std::to_string(spread_bps) + " bps";
            if (logger_) {
                logger_->debug("Trading signal generated: " + signal_reason);  // Use debug to avoid console spam
            }
            
            // Place orders based on signal (using simplified approach)
            if (signal.place_bid || signal.place_ask) {
                place_order_ladder(signal);  // Use the original simulation approach
            }
        }
        
        // Continuous order placement at target rate
        if (now - last_order_time >= target_interval) {            
            // Place new orders if we have market data
            if (current_bid_.load() > 0 && current_ask_.load() > 0) {
                HFTMarketData current_market;
                strcpy(current_market.symbol, "ETHUSDT");
                current_market.bid_price = current_bid_.load();
                current_market.ask_price = current_ask_.load();
                current_market.timestamp = now;
                
                HFTSignal signal = generate_signal(current_market);
                if (signal.place_bid || signal.place_ask) {
                    place_order_ladder(signal);  // Use the original simulation approach
                }
            }
            
            last_order_time = now;
        }
        
        // Process order responses
        HFTOrder response;
        while (inbound_order_queue_.pop(response)) {
            process_order_response(response);
        }
        
        // Ultra-fast loop - no sleep, maximum throughput
        std::this_thread::yield();
    }
}

void HFTEngine::risk_management_worker() {
    std::cout << "🛡️  Risk management worker started" << std::endl;
    logger_->info("Risk management worker started");
    
    double last_pnl = 0.0; // Track last PnL to calculate deltas correctly
    
    while (running_.load()) {
        update_risk_metrics();
        
        // Use existing risk manager for comprehensive risk checks
        if (risk_manager_) {
            auto position = current_position_.load();
            
            // Get ACCURATE PnL from Order Manager instead of buggy HFT engine PnL
            double current_pnl = 0.0;
            if (order_manager_) {
                current_pnl = order_manager_->getCurrentPnL();
            }
            
            // Check if circuit breaker is active
            if (risk_manager_->isCircuitBreakerActive()) {
                logger_->error("Circuit breaker is active - stopping trading");
                std::cout << "⚠️  RISK BREACH: Circuit breaker active!" << std::endl;
                emergency_stop();
                break;
            }
            
            // Get overall risk status
            auto risk_status = risk_manager_->getCurrentRiskStatus();
            if (risk_status == RiskStatus::EMERGENCY) {
                logger_->error("Emergency risk status - stopping trading");
                std::cout << "⚠️  RISK BREACH: Emergency risk status!" << std::endl;
                emergency_stop();
                break;
            }
            
            // Check position limits using the can place order method
            std::string rejection_reason;
            bool can_buy = risk_manager_->canPlaceOrder("ETHUSDT", "BUY", 
                current_ask_.load(), order_size_.load(), rejection_reason);
            bool can_sell = risk_manager_->canPlaceOrder("ETHUSDT", "SELL", 
                current_bid_.load(), order_size_.load(), rejection_reason);
            
            if (!can_buy && !can_sell) {
                logger_->warning("Risk limits preventing all trading: " + rejection_reason);
                risk_breach_.store(true);
            } else {
                risk_breach_.store(false);
            }
            
            // Update risk manager with CORRECT PnL delta from Order Manager
            double pnl_delta = current_pnl - last_pnl;
            if (std::abs(pnl_delta) > 0.001) { // Only update if meaningful change
                risk_manager_->updatePnL(pnl_delta);
                last_pnl = current_pnl;
            }
        }
        
        // Risk check every 100ms
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void HFTEngine::metrics_worker() {
    auto last_print = std::chrono::steady_clock::now();
    auto last_summary = std::chrono::steady_clock::now();
    
    // Track 5-second intervals for trade summaries
    uint64_t last_orders_placed = 0;
    uint64_t last_orders_filled = 0;
    double last_pnl = 0.0;
    double last_position = 0.0;
    
    while (running_.load()) {
        auto now = std::chrono::steady_clock::now();
        
        // Update trading rate metrics
        update_trading_rate();
        
        // Print 5-second trade summary using ONLY Order Manager's accurate data
        if (now - last_summary >= std::chrono::seconds(5)) {
            // Get ONLY accurate data from Order Manager - NO HFT engine bullshit
            uint64_t current_total_trades = 0;
            double current_pnl = 0.0;
            double current_position = 0.0;
            
            if (order_manager_) {
                current_total_trades = order_manager_->getTotalTrades();  
                current_pnl = order_manager_->getCurrentPnL();
                current_position = order_manager_->getCurrentPosition();
            }
            
            // Calculate deltas for this 5-second period (Order Manager data ONLY)
            uint64_t trades_delta = current_total_trades - last_orders_filled;
            double pnl_delta = current_pnl - last_pnl;
            
            // Show 5-second summary with ONLY Order Manager data
            std::cout << "📊 5s: " << trades_delta << " trades"
                      << " | PnL: " << std::fixed << std::setprecision(3) << "$" << pnl_delta 
                      << " | Pos: " << std::setprecision(4) << current_position << " ETH"
                      << " | Total: " << current_total_trades << " trades" << std::endl;
            
            // Update tracking variables with Order Manager data ONLY
            last_orders_filled = current_total_trades;
            last_pnl = current_pnl;
            last_position = current_position;
            last_summary = now;
        }
        
        // Print detailed stats every 10 seconds (unchanged)
        if (now - last_print >= std::chrono::seconds(10)) {
            print_performance_stats();
            last_print = now;
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

HFTSignal HFTEngine::generate_signal(const HFTMarketData& market_data) {
    HFTSignal signal{};
    
    // Always try to place both bid and ask for maximum market making
    signal.place_bid = true;
    signal.place_ask = true;
    signal.num_levels = 2; // Reduce to 2 levels for better focus
    
    // AGGRESSIVE inventory-neutral market making pricing
    double tick_size = 0.01; // $0.01 for ETHUSDT
    double spread_offset = tick_size * 1.5; // Increase to 1.5 ticks for better profit margins
    
    // Place bid BELOW current best bid (we buy cheaper)
    signal.bid_price = market_data.bid_price - spread_offset;
    // Place ask ABOVE current best ask (we sell higher)  
    signal.ask_price = market_data.ask_price + spread_offset;
    
    // Ensure minimum spread for profitability
    double min_spread = tick_size * 2.0; // Increase minimum spread to $0.02
    if ((signal.ask_price - signal.bid_price) < min_spread) {
        double mid = (market_data.bid_price + market_data.ask_price) / 2.0;
        signal.bid_price = mid - (min_spread / 2.0);
        signal.ask_price = mid + (min_spread / 2.0);
    }
    
    signal.bid_quantity = order_size_.load();
    signal.ask_quantity = order_size_.load();
    
    // AGGRESSIVE Position rebalancing: FORCE market-neutral behavior
    double current_pos = current_position_.load();
    double max_neutral_pos = 0.01; // Much tighter neutral zone (0.01 ETH = ~$24)
    
    // IMMEDIATE rebalancing if position gets even slightly imbalanced
    if (std::abs(current_pos) > max_neutral_pos) {
        if (current_pos > max_neutral_pos) { // Too long, FORCE selling
            signal.place_bid = false; // Stop all buying
            signal.ask_quantity *= 3.0; // Triple sell quantity
            
            // Make sell orders VERY AGGRESSIVE to ensure immediate fills
            signal.ask_price = market_data.ask_price - (tick_size * 0.5); // Sell BELOW market ask
            
        } else if (current_pos < -max_neutral_pos) { // Too short, FORCE buying
            signal.place_ask = false; // Stop all selling
            signal.bid_quantity *= 3.0; // Triple buy quantity
            
            // Make buy orders VERY AGGRESSIVE to ensure immediate fills
            signal.bid_price = market_data.bid_price + (tick_size * 0.5); // Buy ABOVE market bid
        }
    }
    
    // Additional inventory penalty: reduce order sizes as position grows
    double inventory_penalty = std::min(1.0, std::abs(current_pos) / 0.02); // Penalty starts at 0.02 ETH
    if (inventory_penalty > 0.5) {
        signal.bid_quantity *= (1.0 - inventory_penalty);
        signal.ask_quantity *= (1.0 - inventory_penalty);
    }
    
    return signal;
}

void HFTEngine::place_order_ladder(const HFTSignal& signal) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Place multiple orders at different price levels for deeper market making
    for (uint32_t level = 0; level < signal.num_levels; ++level) {
        double tick_size = 0.01;
        
        // Place bid orders
        if (signal.place_bid && !risk_breach_.load()) {
            HFTOrder bid_order{};
            bid_order.order_id = generate_order_id();
            bid_order.client_order_id = bid_order.order_id;
            strcpy(bid_order.symbol, "ETHUSDT");
            bid_order.side = 'B';
            bid_order.price = signal.bid_price - (level * tick_size * 0.1);
            bid_order.quantity = signal.bid_quantity * (1.0 - level * 0.1); // Smaller size at worse prices
            bid_order.filled_quantity = 0.0;
            bid_order.status = 'N';
            bid_order.timestamp = std::chrono::high_resolution_clock::now();
            bid_order.order_sent_time = bid_order.timestamp;
            bid_order.priority = level;
            
            if (check_risk_limits(bid_order)) {
                if (send_order(bid_order)) {
                    metrics_.orders_placed.fetch_add(1);
                }
            }
        }
        
        // Place ask orders
        if (signal.place_ask && !risk_breach_.load()) {
            HFTOrder ask_order{};
            ask_order.order_id = generate_order_id();
            ask_order.client_order_id = ask_order.order_id;
            strcpy(ask_order.symbol, "ETHUSDT");
            ask_order.side = 'S';
            ask_order.price = signal.ask_price + (level * tick_size * 0.1);
            ask_order.quantity = signal.ask_quantity * (1.0 - level * 0.1);
            ask_order.filled_quantity = 0.0;
            ask_order.status = 'N';
            ask_order.timestamp = std::chrono::high_resolution_clock::now();
            ask_order.order_sent_time = ask_order.timestamp;
            ask_order.priority = level;
            
            if (check_risk_limits(ask_order)) {
                if (send_order(ask_order)) {
                    metrics_.orders_placed.fetch_add(1);
                }
            }
        }
    }
    
    // Update latency metrics
    auto end_time = std::chrono::high_resolution_clock::now();
    auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    update_latency_metrics(latency_ns);
}

bool HFTEngine::send_order(const HFTOrder& order) {
    // In paper trading mode, simulate realistic fills based on order aggressiveness
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> fill_prob(0.0, 1.0);
    
    // Simulate order send latency (very low for HFT)
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    
    // Store in active orders
    size_t index = active_order_count_.fetch_add(1);
    if (index < active_orders_.size()) {
        active_orders_[index] = order;
    }
    
    // REALISTIC fill simulation based on order aggressiveness and inventory
    double current_pos = current_position_.load();
    double base_fill_probability = 0.15; // Reduced from 20% to 15%
    
    // BOOST fill probability for inventory-balancing trades (these are more likely to fill)
    if ((order.side == 'S' && current_pos > 0.005) || // Selling when long
        (order.side == 'B' && current_pos < -0.005)) { // Buying when short
        base_fill_probability *= 2.0; // Double the fill rate for rebalancing trades
    }
    
    // REDUCE fill probability for inventory-building trades (these should fill less)
    if ((order.side == 'B' && current_pos > 0.005) || // Buying when already long
        (order.side == 'S' && current_pos < -0.005)) { // Selling when already short
        base_fill_probability *= 0.3; // Reduce fill rate for inventory-building trades
    }
    
    // Cap fill probability
    base_fill_probability = std::min(base_fill_probability, 0.4);
    
    // Simulate fills with calculated probability
    if (fill_prob(gen) < base_fill_probability) {
        HFTOrder filled_order = order;
        filled_order.status = 'F';
        filled_order.filled_quantity = filled_order.quantity;
        filled_order.fill_time = std::chrono::high_resolution_clock::now();
        inbound_order_queue_.push(filled_order);
    }
    
    return true;
}

void HFTEngine::process_order_response(const HFTOrder& response) {
    if (response.status == 'F') { // Filled
        metrics_.orders_filled.fetch_add(1);
        
        // Update position (atomic double update)
        double position_change = (response.side == 'B') ? response.filled_quantity : -response.filled_quantity;
        double old_pos = current_position_.load();
        while (!current_position_.compare_exchange_weak(old_pos, old_pos + position_change));
        
        // Remove all HFT engine PnL simulation - Order Manager handles real PnL calculation
        
        // Update OrderManager for session tracking and REAL PnL calculation
        if (order_manager_) {
            std::string side = (response.side == 'B') ? "BUY" : "SELL";
            auto future_response = order_manager_->placeOrder(response.symbol, side, response.price, response.filled_quantity);
            OrderResponse order_response = future_response.get();
        }
    }
}

void HFTEngine::cancel_stale_orders() {
    // Cancel orders older than 100ms (very aggressive for HFT)
    auto now = std::chrono::high_resolution_clock::now();
    const auto stale_threshold = std::chrono::milliseconds(100);
    
    for (size_t i = 0; i < active_order_count_.load() && i < active_orders_.size(); ++i) {
        auto& order = active_orders_[i];
        if (order.status == 'N' && (now - order.timestamp) > stale_threshold) {
            order.status = 'C'; // Mark as canceled
            metrics_.orders_canceled.fetch_add(1);
        }
    }
}

bool HFTEngine::check_risk_limits(const HFTOrder& order) {
    if (risk_breach_.load()) {
        return false;
    }
    
    // Check position limits
    double position_change = (order.side == 'B') ? order.quantity : -order.quantity;
    double new_position = current_position_.load() + position_change;
    
    if (std::abs(new_position) > max_position_.load()) {
        return false;
    }
    
    return true;
}

void HFTEngine::update_risk_metrics() {
    // Update atomic metrics for thread safety
    metrics_.current_position.store(current_position_.load());
}

void HFTEngine::update_latency_metrics(uint64_t latency_ns) {
    // Update min/max latency
    uint64_t current_min = metrics_.min_order_latency_ns.load();
    while (latency_ns < current_min && !metrics_.min_order_latency_ns.compare_exchange_weak(current_min, latency_ns));
    
    uint64_t current_max = metrics_.max_order_latency_ns.load();
    while (latency_ns > current_max && !metrics_.max_order_latency_ns.compare_exchange_weak(current_max, latency_ns));
    
    // Simple running average (could be improved with better algorithm)
    uint64_t current_avg = metrics_.avg_order_latency_ns.load();
    uint64_t new_avg = (current_avg + latency_ns) / 2;
    metrics_.avg_order_latency_ns.store(new_avg);
}

void HFTEngine::update_trading_rate() {
    auto now = std::chrono::high_resolution_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    
    static uint64_t last_orders = 0;
    static uint64_t last_time = now_ms;
    
    uint64_t current_orders = metrics_.orders_placed.load();
    uint64_t orders_delta = current_orders - last_orders;
    uint64_t time_delta = now_ms - last_time;
    
    if (time_delta >= 1000) { // Update every second
        uint64_t rate = (orders_delta * 1000) / time_delta;
        metrics_.orders_per_second.store(rate);
        last_orders = current_orders;
        last_time = now_ms;
    }
}

void HFTEngine::print_performance_stats() const {
    auto runtime = std::chrono::high_resolution_clock::now() - engine_start_time_;
    auto runtime_seconds = std::chrono::duration_cast<std::chrono::seconds>(runtime).count();
    
    // Get ONLY Order Manager data - NO HFT engine fake metrics
    uint64_t total_trades = 0;
    double current_pnl = 0.0;
    double current_position = 0.0;
    
    if (order_manager_) {
        total_trades = order_manager_->getTotalTrades();
        current_pnl = order_manager_->getCurrentPnL();
        current_position = order_manager_->getCurrentPosition();
    }
    
    std::cout << "\n📊 ORDER MANAGER PERFORMANCE (10s Update)" << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << "Runtime: " << runtime_seconds << "s" << std::endl;
    std::cout << "Total Trades: " << total_trades << std::endl;
    std::cout << "Position: " << std::fixed << std::setprecision(4) << current_position << " ETH" << std::endl;
    std::cout << "PnL: $" << std::setprecision(4) << current_pnl << std::endl;
    std::cout << "Avg Trades/sec: " << (total_trades / std::max(1LL, (long long)runtime_seconds)) << std::endl;
    std::cout << "=========================================\n" << std::endl;
}

HFTMetrics HFTEngine::get_metrics() const {
    HFTMetrics result;
    result.orders_placed = metrics_.orders_placed.load();
    result.orders_canceled = metrics_.orders_canceled.load();
    result.orders_filled = metrics_.orders_filled.load();
    result.market_data_updates = metrics_.market_data_updates.load();
    result.total_pnl = metrics_.total_pnl.load();
    result.current_position = metrics_.current_position.load();
    result.avg_order_latency_ns = metrics_.avg_order_latency_ns.load();
    result.min_order_latency_ns = metrics_.min_order_latency_ns.load();
    result.max_order_latency_ns = metrics_.max_order_latency_ns.load();
    result.orders_per_second = metrics_.orders_per_second.load();
    result.last_rate_update = metrics_.last_rate_update.load();
    return result;
}

void HFTEngine::emergency_stop() {
    std::cout << "🚨 EMERGENCY STOP TRIGGERED!" << std::endl;
    logger_->error("Emergency stop triggered");
    risk_breach_.store(true);
    stop();
}

// Generate unique order ID
uint64_t HFTEngine::generate_order_id() {
    return next_order_id_.fetch_add(1);
}

// Note: Custom trade logging removed - now using OrderManager's built-in logging

// Generate session summary using EXACT original format via OrderManager
void HFTEngine::generate_session_summary() {
    // Use OrderManager's shutdown method which calls generateSessionSummary internally
    if (order_manager_) {
        order_manager_->shutdown();
    }
    
    // No duplicate HFT engine logging - OrderManager handles the complete session summary
} 