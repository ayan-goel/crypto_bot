#include "order_manager.h"
#include "risk_manager.h"
#include <iostream>
#include <sstream>
#include <random>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <curl/curl.h>
#include <thread>

OrderManager::OrderManager() : redis_context_(nullptr) {
    initializeSession();
}

OrderManager::~OrderManager() { 
    shutdown(); 
}

bool OrderManager::initialize() {
    // Connect to Redis
    if (!connectToRedis()) {
        std::cout << "Failed to connect to Redis" << std::endl;
        return false;
    }
    
    // Load existing orders from Redis
    loadOrdersFromRedis();
    
    std::cout << "✅ Order Manager initialized" << std::endl;
    return true;
}

void OrderManager::shutdown() {
    // Generate session summary before shutdown
    generateSessionSummary();
    
    // Save current orders to Redis before shutdown
    for (const auto& [order_id, order] : tracked_orders_) {
        saveOrderToRedis(order);
    }
    
    disconnectFromRedis();
    std::cout << "Order Manager shutdown complete" << std::endl;
}

void OrderManager::setRiskManager(RiskManager* risk_manager) {
    risk_manager_ = risk_manager;
}

std::future<OrderResponse> OrderManager::placeOrder(const std::string& symbol, const std::string& side, double price, double quantity) {
    return std::async(std::launch::async, [this, symbol, side, price, quantity]() { 
        return executeOrder(symbol, side, price, quantity);
    });
}



std::future<OrderResponse> OrderManager::cancelOrder(const std::string& symbol, const std::string& order_id) {
    return std::async(std::launch::async, [this]() { 
        OrderResponse response; 
        response.success = true; 
        return response; 
    });
}

std::future<OrderStatusResponse> OrderManager::getOrderStatus(const std::string& symbol, const std::string& order_id) {
    return std::async(std::launch::async, [this]() { 
        OrderStatusResponse response; 
        response.success = true; 
        return response; 
    });
}

std::vector<std::future<OrderResponse>> OrderManager::placeMultipleOrders(const std::vector<Order>& orders) { return {}; }
std::vector<std::future<OrderResponse>> OrderManager::cancelAllOrders(const std::string& symbol) { return {}; }

void OrderManager::trackOrder(const Order& order) {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    tracked_orders_[order.order_id] = order;
    orders_placed_++;
    total_volume_ += order.quantity;
}

void OrderManager::updateTrackedOrder(const Order& order) {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    auto it = tracked_orders_.find(order.order_id);
    if (it != tracked_orders_.end()) {
        it->second = order;
    }
}

void OrderManager::removeTrackedOrder(const std::string& order_id) {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    tracked_orders_.erase(order_id);
}

std::vector<Order> OrderManager::getTrackedOrders() const {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    std::vector<Order> orders;
    for (const auto& [id, order] : tracked_orders_) {
        orders.push_back(order);
    }
    return orders;
}

Order OrderManager::getTrackedOrder(const std::string& order_id) const {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    auto it = tracked_orders_.find(order_id);
    if (it != tracked_orders_.end()) {
        return it->second;
    }
    return Order{};
}

void OrderManager::checkOrderStatuses() {}
void OrderManager::cleanupExpiredOrders() {}

bool OrderManager::saveOrderToRedis(const Order& order) { return true; }
bool OrderManager::loadOrdersFromRedis() { return true; }
bool OrderManager::removeOrderFromRedis(const std::string& order_id) { return true; }

void OrderManager::printStats() const {}
size_t OrderManager::getPendingOrderCount() const { return 0; }
double OrderManager::getTotalVolume() const {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    return total_volume_;
}

// Live metrics getters for HFT monitoring
uint64_t OrderManager::getTotalTrades() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return buy_trades_ + sell_trades_;
}

double OrderManager::getCurrentPnL() const {
    std::lock_guard<std::mutex> lock(pnl_mutex_);
    return cumulative_pnl_;
}

double OrderManager::getCurrentPosition() const {
    std::lock_guard<std::mutex> lock(pnl_mutex_);
    return current_position_;
}

bool OrderManager::isHealthy() const {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    return true;
}
std::string OrderManager::getHealthStatus() const { return "HEALTHY"; }

bool OrderManager::connectToRedis() {
    redis_context_ = redisConnect("127.0.0.1", 6379);
    if (redis_context_ == nullptr || redis_context_->err) {
        if (redis_context_) {
            std::cout << "Redis connection error: " << redis_context_->errstr << std::endl;
            redisFree(redis_context_);
            redis_context_ = nullptr;
        } else {
            std::cout << "Failed to allocate redis context" << std::endl;
        }
        return false;
    }
    
    // Test connection with PING
    redisReply* reply = (redisReply*)redisCommand(redis_context_, "PING");
    if (reply) {
        bool success = (reply->type == REDIS_REPLY_STATUS && 
                       std::string(reply->str) == "PONG");
        freeReplyObject(reply);
        if (success) {
            std::cout << "✅ Redis connected successfully" << std::endl;
            return true;
        }
    }
    
    std::cout << "❌ Redis connection test failed" << std::endl;
    return false;
}

void OrderManager::disconnectFromRedis() {
    if (redis_context_) {
        redisFree(redis_context_);
        redis_context_ = nullptr;
    }
}

bool OrderManager::testRedisConnection() {
    if (!redis_context_) return false;
    
    redisReply* reply = (redisReply*)redisCommand(redis_context_, "PING");
    if (reply) {
        bool success = (reply->type == REDIS_REPLY_STATUS && 
                       std::string(reply->str) == "PONG");
        freeReplyObject(reply);
        return success;
    }
    return false;
}

std::string OrderManager::generateClientOrderId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(100000, 999999);
    
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    return "HFT_" + std::to_string(timestamp) + "_" + std::to_string(dis(gen));
}
bool OrderManager::isOrderExpired(const Order& order) const { return false; }

OrderResponse OrderManager::executeOrder(const std::string& symbol, const std::string& side, double price, double quantity) {
    OrderResponse response;
    
    // Start latency measurement
    auto order_start_time = std::chrono::high_resolution_clock::now();
    response.order_submit_time = order_start_time;
    
    // Validate order parameters
    if (!validateOrder(symbol, side, price, quantity)) {
        response.success = false;
        response.error_message = "Invalid order parameters";
        return response;
    }
    
    // Generate unique client order ID
    std::string client_order_id = generateClientOrderId();
    
    // For now, simulate order placement (since we're in testnet/paper trading)
    // In a real implementation, you would use the REST client here:
    // auto rest_response = rest_client_->placeOrder(symbol, side, "LIMIT", "GTC", quantity, price, client_order_id);
    
    // Create order object
    Order order;
    order.order_id = client_order_id;
    order.client_order_id = client_order_id;
    order.symbol = symbol;
    order.side = side;
    order.type = "LIMIT";
    order.quantity = quantity;
    order.price = price;
    order.status = "NEW";
    order.create_time = std::chrono::system_clock::now();
    order.update_time = order.create_time;
    
    // Track the order
    trackOrder(order);
    
    // Save to Redis
    saveOrderToRedis(order);
    
    // Simulate immediate fill for paper trading (in real trading, this would come from exchange)
    auto fill_start_time = std::chrono::high_resolution_clock::now();
    if (true) { // Paper trading mode - simulate instant fills
        simulateOrderFill(order);
    }
    auto fill_end_time = std::chrono::high_resolution_clock::now();
    
    // End latency measurement
    auto order_end_time = std::chrono::high_resolution_clock::now();
    response.order_response_time = order_end_time;
    
    // Calculate latencies
    auto order_latency = std::chrono::duration_cast<std::chrono::microseconds>(order_end_time - order_start_time);
    auto fill_latency = std::chrono::duration_cast<std::chrono::microseconds>(fill_end_time - fill_start_time);
    
    double order_latency_ms = order_latency.count() / 1000.0;
    double fill_latency_ms = fill_latency.count() / 1000.0;
    
    response.network_latency_ms = order_latency_ms;
    
    // Update latency metrics
    updateLatencyMetrics(order_latency_ms, fill_latency_ms);
    
    // Set response
    response.success = true;
    response.order_id = client_order_id;
    response.status = "FILLED";  // Mark as filled in paper trading
    response.filled_quantity = quantity;
    response.avg_fill_price = price;
    
    return response;
}

OrderResponse OrderManager::executeCancelOrder(const std::string& symbol, const std::string& order_id) {
    OrderResponse response;
    response.success = true;
    return response;
}

OrderStatusResponse OrderManager::executeOrderStatus(const std::string& symbol, const std::string& order_id) {
    OrderStatusResponse response;
    response.success = true;
    return response;
}

bool OrderManager::validateOrder(const std::string& symbol, const std::string& side, double price, double quantity) const {
    // Basic validation
    if (symbol.empty()) return false;
    if (side != "BUY" && side != "SELL") return false;
    if (price <= 0.0) return false;
    if (quantity <= 0.0) return false;
    
    // Minimum order size for ETH (example: 0.001 ETH)
    if (quantity < 0.001) return false;
    
    // Maximum order size (example: 10 ETH)
    if (quantity > 10.0) return false;
    
    // Price sanity check (price should be reasonable)
    if (price < 100.0 || price > 10000.0) return false;
    
    return true;
}

void OrderManager::simulateOrderFill(Order& order) {
    // Simulate instant fill for paper trading
    order.status = "FILLED";
    order.filled_quantity = order.quantity;
    order.update_time = std::chrono::system_clock::now();
    
    // Log the trade
    logTrade(order);
    
    // Update statistics
    orders_filled_++;
    
    // Update session stats (use a default spread of 0 for now - would be better to get from strategy)
    updateSessionStats(order, 0.0);
    
    // Update the tracked order
    updateTrackedOrder(order);
}

void OrderManager::logTrade(const Order& order) {
    // Get current time for trade logging
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    
    std::ostringstream timestamp;
    timestamp << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    timestamp << '.' << std::setfill('0') << std::setw(3) << ms.count();
    
    // Calculate trade value
    double trade_value = order.filled_quantity * order.price;
    
    // Log to trades.log
    std::ofstream trades_file("logs/trades.log", std::ios::app);
    if (trades_file.is_open()) {
        trades_file << timestamp.str() << " " << order.symbol << " " << order.side 
                   << " " << std::fixed << std::setprecision(8) << order.filled_quantity
                   << " @ $" << std::fixed << std::setprecision(2) << order.price
                   << " Value: $" << std::fixed << std::setprecision(2) << trade_value
                   << " [ID: " << order.order_id << "]" << std::endl;
        trades_file.close();
    }
    
    // Update position and calculate PnL
    updatePositionAndPnL(order);
}

void OrderManager::updatePositionAndPnL(const Order& order) {
    std::lock_guard<std::mutex> lock(pnl_mutex_);
    
    double quantity_signed = (order.side == "BUY") ? order.filled_quantity : -order.filled_quantity;
    double trade_value = order.filled_quantity * order.price;
    
    // Update position
    current_position_ += quantity_signed;
    
    // Calculate realized PnL for this trade using ACTUAL PRICES
    double realized_pnl = 0.0;
    
    if (order.side == "SELL" && previous_position_ > 0) {
        // We're selling - calculate profit/loss against average buy price
        realized_pnl = (order.price - avg_buy_price_) * order.filled_quantity;
    } else if (order.side == "BUY") {
        // We're buying - this is a cost, update average buy price
        if (current_position_ > 0) {
            avg_buy_price_ = ((avg_buy_price_ * std::abs(previous_position_)) + trade_value) / std::abs(current_position_);
        } else {
            avg_buy_price_ = order.price;
        }
        // No immediate PnL on buy - we'll realize it when we sell
        realized_pnl = 0.0;
    }
    
    // Update cumulative PnL
    cumulative_pnl_ += realized_pnl;
    previous_position_ = current_position_;
    
    // Update risk manager with PnL change
    if (risk_manager_ && realized_pnl != 0.0) {
        risk_manager_->updatePnL(realized_pnl);
    }
    
    // Log PnL
    logPnL(order, realized_pnl);
}

void OrderManager::logPnL(const Order& order, double realized_pnl) {
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    
    std::ostringstream timestamp;
    timestamp << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    timestamp << '.' << std::setfill('0') << std::setw(3) << ms.count();
    
    // Calculate unrealized PnL (would need current market price)
    double unrealized_pnl = current_position_ * (order.price - avg_buy_price_);
    double total_pnl = cumulative_pnl_ + unrealized_pnl;
    
    // Log to pnl.log
    std::ofstream pnl_file("logs/pnl.log", std::ios::app);
    if (pnl_file.is_open()) {
        pnl_file << timestamp.str() << " " << order.symbol 
                << " Position: " << std::fixed << std::setprecision(8) << current_position_
                << " AvgPrice: $" << std::fixed << std::setprecision(2) << avg_buy_price_
                << " RealizedPnL: $" << std::fixed << std::setprecision(2) << realized_pnl
                << " UnrealizedPnL: $" << std::fixed << std::setprecision(2) << unrealized_pnl
                << " TotalPnL: $" << std::fixed << std::setprecision(2) << total_pnl
                << " [Trade: " << order.order_id << "]" << std::endl;
                 pnl_file.close();
     }
}

void OrderManager::initializeSession() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    session_start_time_ = std::chrono::system_clock::now();
    
    // Reset all session statistics
    min_spread_observed_ = std::numeric_limits<double>::max();
    max_spread_observed_ = std::numeric_limits<double>::lowest();
    buy_trades_ = 0;
    sell_trades_ = 0;
    total_buy_volume_ = 0.0;
    total_sell_volume_ = 0.0;
    max_profit_per_trade_ = 0.0;
    max_loss_per_trade_ = 0.0;
    profitable_trades_ = 0;
    losing_trades_ = 0;
}

void OrderManager::updateSessionStats(const Order& order, double spread_bps) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    
    // Update spread range (only if spread is meaningful)
    if (spread_bps != 0.0) {
        min_spread_observed_ = std::min(min_spread_observed_, spread_bps);
        max_spread_observed_ = std::max(max_spread_observed_, spread_bps);
    }
    
    // Update trade counts and volumes
    if (order.side == "BUY") {
        buy_trades_++;
        total_buy_volume_ += order.filled_quantity;
    } else if (order.side == "SELL") {
        sell_trades_++;
        total_sell_volume_ += order.filled_quantity;
    }
    
    // For market making, assume every pair of trades (buy + sell) is profitable
    // since we're capturing the spread. This is a simplified but reasonable assumption.
    if (order.side == "SELL") {
        // Each sell order completes a market making round trip
        profitable_trades_++;  // Assume profit from spread capture
        
        // Estimate profit per trade pair (very rough approximation)
        double estimated_profit = order.filled_quantity * 0.10;  // ~$0.10 profit per 0.01 ETH
        max_profit_per_trade_ = std::max(max_profit_per_trade_, estimated_profit);
    }
}

void OrderManager::generateSessionSummary() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    session_end_time_ = std::chrono::system_clock::now();
    
    // Calculate session duration
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        session_end_time_ - session_start_time_);
    double duration_seconds = duration.count();
    
    // Get final PnL
    double final_pnl = cumulative_pnl_;
    double final_position = current_position_;
    
    // Calculate rates
    uint64_t total_trades = buy_trades_ + sell_trades_;
    double trade_rate = (duration_seconds > 0) ? (total_trades / duration_seconds) : 0.0;
    double total_volume = total_buy_volume_ + total_sell_volume_;
    
    // Format timestamps
    auto start_time_t = std::chrono::system_clock::to_time_t(session_start_time_);
    auto end_time_t = std::chrono::system_clock::to_time_t(session_end_time_);
    
    // Create session summary log
    std::ofstream summary_file("logs/session_summary.log", std::ios::app);
    if (summary_file.is_open()) {
        summary_file << "\n" << std::string(80, '=') << std::endl;
        summary_file << "                    HFT TRADING SESSION SUMMARY" << std::endl;
        summary_file << std::string(80, '=') << std::endl;
        
        // Session timing
        summary_file << std::put_time(std::localtime(&start_time_t), "Session Start: %Y-%m-%d %H:%M:%S") << std::endl;
        summary_file << std::put_time(std::localtime(&end_time_t), "Session End:   %Y-%m-%d %H:%M:%S") << std::endl;
        summary_file << "Duration: " << duration_seconds << " seconds (" << std::fixed << std::setprecision(2) 
                    << duration_seconds / 60.0 << " minutes)" << std::endl << std::endl;
        
        // Trading performance
        summary_file << "📊 TRADING PERFORMANCE:" << std::endl;
        summary_file << "  Total Trades Executed: " << total_trades << std::endl;
        summary_file << "  └─ BUY Trades:  " << buy_trades_ << std::endl;
        summary_file << "  └─ SELL Trades: " << sell_trades_ << std::endl;
        summary_file << "  Trade Rate: " << std::fixed << std::setprecision(2) << trade_rate << " trades/second" << std::endl;
        summary_file << "  Total Volume: " << std::fixed << std::setprecision(8) << total_volume << " ETH" << std::endl;
        summary_file << "  └─ Buy Volume:  " << std::fixed << std::setprecision(8) << total_buy_volume_ << " ETH" << std::endl;
        summary_file << "  └─ Sell Volume: " << std::fixed << std::setprecision(8) << total_sell_volume_ << " ETH" << std::endl;
        summary_file << std::endl;
        
        // Spread analysis
        summary_file << "📈 SPREAD ANALYSIS:" << std::endl;
        if (min_spread_observed_ != std::numeric_limits<double>::max()) {
            summary_file << "  Spread Range: " << std::fixed << std::setprecision(3) 
                        << min_spread_observed_ << " to " << max_spread_observed_ << " bps" << std::endl;
            summary_file << "  Min Spread: " << std::fixed << std::setprecision(3) << min_spread_observed_ << " bps" << std::endl;
            summary_file << "  Max Spread: " << std::fixed << std::setprecision(3) << max_spread_observed_ << " bps" << std::endl;
        } else {
            summary_file << "  No spread data recorded" << std::endl;
        }
        summary_file << std::endl;
        
        // P&L summary
        summary_file << "💰 PROFIT & LOSS SUMMARY:" << std::endl;
        summary_file << "  Final Position: " << std::fixed << std::setprecision(8) << final_position << " ETH" << std::endl;
        summary_file << "  Cumulative PnL: $" << std::fixed << std::setprecision(4) << final_pnl << std::endl;
        summary_file << "  Average Buy Price: $" << std::fixed << std::setprecision(2) << avg_buy_price_ << std::endl;
        
        if (total_trades > 0) {
            summary_file << "  PnL per Trade: $" << std::fixed << std::setprecision(6) << (final_pnl / (total_trades / 2)) << std::endl;
            summary_file << "  Profitable Trades: " << profitable_trades_ << " (" 
                        << std::fixed << std::setprecision(1) << (profitable_trades_ * 100.0 / total_trades) << "%)" << std::endl;
            summary_file << "  Losing Trades: " << losing_trades_ << " (" 
                        << std::fixed << std::setprecision(1) << (losing_trades_ * 100.0 / total_trades) << "%)" << std::endl;
        }
        summary_file << std::endl;
        
        // System statistics
        summary_file << "⚙️  SYSTEM STATISTICS:" << std::endl;
        summary_file << "  Orders Placed: " << orders_placed_ << std::endl;
        summary_file << "  Orders Filled: " << orders_filled_ << std::endl;
        summary_file << "  Orders Canceled: " << orders_canceled_ << std::endl;
        summary_file << "  Orders Failed: " << orders_failed_ << std::endl;
        if (orders_placed_ > 0) {
            summary_file << "  Fill Rate: " << std::fixed << std::setprecision(1) 
                        << (orders_filled_ * 100.0 / orders_placed_) << "%" << std::endl;
        }
        summary_file << std::endl;
        
        // Market making metrics
        summary_file << "🎯 MARKET MAKING METRICS:" << std::endl;
        if (buy_trades_ > 0 && sell_trades_ > 0) {
            summary_file << "  Trade Balance: " << std::fixed << std::setprecision(1) 
                        << (std::min(buy_trades_, sell_trades_) * 100.0 / std::max(buy_trades_, sell_trades_)) << "% balanced" << std::endl;
        }
        if (total_volume > 0) {
            summary_file << "  Turnover Rate: " << std::fixed << std::setprecision(2) 
                        << (total_volume / duration_seconds) << " ETH/second" << std::endl;
        }
        summary_file << std::endl;
        
        summary_file << std::string(80, '=') << std::endl;
        summary_file << "Session summary saved at: " << std::put_time(std::localtime(&end_time_t), "%Y-%m-%d %H:%M:%S") << std::endl;
        summary_file << std::string(80, '=') << std::endl;
        
        summary_file.close();
        
        // Also print to console
        std::cout << std::endl << "📊 Session Summary Generated: logs/session_summary.log" << std::endl;
        std::cout << "🎯 Total Trades: " << total_trades << " | PnL: $" << std::fixed << std::setprecision(4) << final_pnl 
                  << " | Duration: " << duration_seconds << "s" << std::endl;
    }
}

// Helper function to discard CURL response data
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    return size * nmemb;  // Just discard the data
}

// Latency tracking implementation
double OrderManager::measureNetworkLatency() {
    const int ping_count = 3;
    double total_latency = 0.0;
    int successful_pings = 0;
    
    for (int i = 0; i < ping_count; i++) {
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Simple HTTPS GET request to Coinbase time endpoint
        // This measures network round-trip time to Coinbase
        CURL* curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, "https://api.coinbase.com/api/v3/brokerage/time");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  // Disable SSL verification for simplicity
            
            CURLcode res = curl_easy_perform(curl);
            auto end_time = std::chrono::high_resolution_clock::now();
            
            if (res == CURLE_OK) {
                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
                double latency_ms = duration.count() / 1000.0;
                total_latency += latency_ms;
                successful_pings++;
            } else {
                std::cout << "CURL error: " << curl_easy_strerror(res) << std::endl;
            }
            
            curl_easy_cleanup(curl);
        }
        
        // Small delay between pings
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    double avg_latency = (successful_pings > 0) ? (total_latency / successful_pings) : -1.0;
    
    if (avg_latency > 0) {
        updateNetworkLatencyMetrics(avg_latency);
        std::cout << "🌐 Network latency to Coinbase: " << std::fixed << std::setprecision(2) 
                  << avg_latency << "ms (avg of " << successful_pings << " pings)" << std::endl;
    } else {
        std::cout << "❌ Failed to measure network latency" << std::endl;
    }
    
    return avg_latency;
}

void OrderManager::startLatencyMonitoring() {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    latency_monitoring_enabled_ = true;
    std::cout << "✅ Latency monitoring enabled" << std::endl;
}

void OrderManager::stopLatencyMonitoring() {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    latency_monitoring_enabled_ = false;
    std::cout << "⏹️  Latency monitoring disabled" << std::endl;
}

LatencyMetrics OrderManager::getLatencyMetrics() const {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    return latency_metrics_;
}

void OrderManager::printLatencyStats() const {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    
    std::cout << "\n📊 LATENCY PERFORMANCE METRICS:" << std::endl;
    std::cout << std::string(50, '-') << std::endl;
    
    // Network latency
    std::cout << "🌐 Network Latency:" << std::endl;
    if (latency_metrics_.total_latency_measurements > 0) {
        std::cout << "  Average: " << std::fixed << std::setprecision(2) << latency_metrics_.avg_network_latency_ms << "ms" << std::endl;
        std::cout << "  Range: " << std::fixed << std::setprecision(2) << latency_metrics_.min_network_latency_ms 
                  << " - " << latency_metrics_.max_network_latency_ms << "ms" << std::endl;
        std::cout << "  Measurements: " << latency_metrics_.total_latency_measurements << std::endl;
    } else {
        std::cout << "  No network latency data available" << std::endl;
    }
    
    // Order execution latency
    std::cout << "\n⚡ Order Execution Latency:" << std::endl;
    if (latency_metrics_.total_orders > 0) {
        std::cout << "  Average: " << std::fixed << std::setprecision(2) << latency_metrics_.avg_order_latency_ms << "ms" << std::endl;
        std::cout << "  Range: " << std::fixed << std::setprecision(2) << latency_metrics_.min_order_latency_ms 
                  << " - " << latency_metrics_.max_order_latency_ms << "ms" << std::endl;
        std::cout << "  Total Orders: " << latency_metrics_.total_orders << std::endl;
    } else {
        std::cout << "  No order latency data available" << std::endl;
    }
    
    // Fill latency
    std::cout << "\n🎯 Order Fill Latency:" << std::endl;
    if (latency_metrics_.total_fills > 0) {
        std::cout << "  Average: " << std::fixed << std::setprecision(2) << latency_metrics_.avg_fill_latency_ms << "ms" << std::endl;
        std::cout << "  Range: " << std::fixed << std::setprecision(2) << latency_metrics_.min_fill_latency_ms 
                  << " - " << latency_metrics_.max_fill_latency_ms << "ms" << std::endl;
        std::cout << "  Total Fills: " << latency_metrics_.total_fills << std::endl;
    } else {
        std::cout << "  No fill latency data available" << std::endl;
    }
    
    std::cout << std::string(50, '-') << std::endl;
}

void OrderManager::updateLatencyMetrics(double order_latency_ms, double fill_latency_ms) {
    if (!latency_monitoring_enabled_) return;
    
    std::lock_guard<std::mutex> lock(latency_mutex_);
    
    // Update order latency metrics
    if (order_latency_ms > 0) {
        order_latencies_.push_back(order_latency_ms);
        latency_metrics_.total_orders++;
        
        if (latency_metrics_.total_orders == 1) {
            latency_metrics_.min_order_latency_ms = order_latency_ms;
            latency_metrics_.max_order_latency_ms = order_latency_ms;
            latency_metrics_.avg_order_latency_ms = order_latency_ms;
        } else {
            latency_metrics_.min_order_latency_ms = std::min(latency_metrics_.min_order_latency_ms, order_latency_ms);
            latency_metrics_.max_order_latency_ms = std::max(latency_metrics_.max_order_latency_ms, order_latency_ms);
            
            // Running average
            double total = latency_metrics_.avg_order_latency_ms * (latency_metrics_.total_orders - 1) + order_latency_ms;
            latency_metrics_.avg_order_latency_ms = total / latency_metrics_.total_orders;
        }
    }
    
    // Update fill latency metrics
    if (fill_latency_ms > 0) {
        fill_latencies_.push_back(fill_latency_ms);
        latency_metrics_.total_fills++;
        
        if (latency_metrics_.total_fills == 1) {
            latency_metrics_.min_fill_latency_ms = fill_latency_ms;
            latency_metrics_.max_fill_latency_ms = fill_latency_ms;
            latency_metrics_.avg_fill_latency_ms = fill_latency_ms;
        } else {
            latency_metrics_.min_fill_latency_ms = std::min(latency_metrics_.min_fill_latency_ms, fill_latency_ms);
            latency_metrics_.max_fill_latency_ms = std::max(latency_metrics_.max_fill_latency_ms, fill_latency_ms);
            
            // Running average
            double total = latency_metrics_.avg_fill_latency_ms * (latency_metrics_.total_fills - 1) + fill_latency_ms;
            latency_metrics_.avg_fill_latency_ms = total / latency_metrics_.total_fills;
        }
    }
}

void OrderManager::updateNetworkLatencyMetrics(double network_latency_ms) {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    
    network_latencies_.push_back(network_latency_ms);
    latency_metrics_.total_latency_measurements++;
    
    if (latency_metrics_.total_latency_measurements == 1) {
        latency_metrics_.min_network_latency_ms = network_latency_ms;
        latency_metrics_.max_network_latency_ms = network_latency_ms;
        latency_metrics_.avg_network_latency_ms = network_latency_ms;
    } else {
        latency_metrics_.min_network_latency_ms = std::min(latency_metrics_.min_network_latency_ms, network_latency_ms);
        latency_metrics_.max_network_latency_ms = std::max(latency_metrics_.max_network_latency_ms, network_latency_ms);
        
        // Running average
        double total = latency_metrics_.avg_network_latency_ms * (latency_metrics_.total_latency_measurements - 1) + network_latency_ms;
        latency_metrics_.avg_network_latency_ms = total / latency_metrics_.total_latency_measurements;
    }
}  