#pragma once

#include "strategy.h"
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <future>
#include <limits>
#include <chrono>
#include <hiredis/hiredis.h>

// Forward declaration
class RiskManager;

struct OrderResponse {
    bool success = false;
    std::string order_id;
    std::string status;
    std::string error_message;
    double filled_quantity = 0.0;
    double avg_fill_price = 0.0;
    
    // Latency metrics
    std::chrono::high_resolution_clock::time_point order_submit_time;
    std::chrono::high_resolution_clock::time_point order_response_time;
    double network_latency_ms = 0.0;
};

struct OrderStatusResponse {
    bool success = false;
    Order order;
    std::string error_message;
};

struct LatencyMetrics {
    double avg_order_latency_ms = 0.0;
    double min_order_latency_ms = std::numeric_limits<double>::max();
    double max_order_latency_ms = 0.0;
    double avg_fill_latency_ms = 0.0;
    double min_fill_latency_ms = std::numeric_limits<double>::max();
    double max_fill_latency_ms = 0.0;
    double avg_network_latency_ms = 0.0;
    double min_network_latency_ms = std::numeric_limits<double>::max();
    double max_network_latency_ms = 0.0;
    uint64_t total_orders = 0;
    uint64_t total_fills = 0;
    uint64_t total_latency_measurements = 0;
};

class OrderManager {
public:
    OrderManager();
    ~OrderManager();
    
    // Initialization
    bool initialize();
    void shutdown();
    
    // Latency testing
    double measureNetworkLatency();
    void startLatencyMonitoring();
    void stopLatencyMonitoring();
    LatencyMetrics getLatencyMetrics() const;
    void printLatencyStats() const;
    
    // Order operations
    std::future<OrderResponse> placeOrder(const std::string& symbol, 
                                        const std::string& side, 
                                        double price, 
                                        double quantity);
    
    std::future<OrderResponse> cancelOrder(const std::string& symbol, 
                                         const std::string& order_id);
    
    std::future<OrderStatusResponse> getOrderStatus(const std::string& symbol, 
                                                   const std::string& order_id);
    
    // Batch operations
    std::vector<std::future<OrderResponse>> placeMultipleOrders(const std::vector<Order>& orders);
    std::vector<std::future<OrderResponse>> cancelAllOrders(const std::string& symbol);
    
    // Order tracking
    void trackOrder(const Order& order);
    void updateTrackedOrder(const Order& order);
    void removeTrackedOrder(const std::string& order_id);
    std::vector<Order> getTrackedOrders() const;
    Order getTrackedOrder(const std::string& order_id) const;
    
    // Periodic operations
    void checkOrderStatuses();  // Check all pending orders
    void cleanupExpiredOrders();
    
    // Redis operations
    bool saveOrderToRedis(const Order& order);
    bool loadOrdersFromRedis();
    bool removeOrderFromRedis(const std::string& order_id);
    
    // Statistics
    void printStats() const;
    size_t getPendingOrderCount() const;
    double getTotalVolume() const;
    
    // Live metrics getters for HFT monitoring
    uint64_t getTotalTrades() const;
    double getCurrentPnL() const;
    double getCurrentPosition() const;
    
    // Health monitoring
    bool isHealthy() const;
    std::string getHealthStatus() const;
    
    // Risk management integration
    void setRiskManager(RiskManager* risk_manager);

private:
    // Redis connection
    redisContext* redis_context_ = nullptr;
    std::string redis_host_;
    int redis_port_;
    int redis_db_;
    
    // Order tracking
    std::map<std::string, Order> tracked_orders_;
    mutable std::mutex orders_mutex_;
    
    // Statistics
    uint64_t orders_placed_ = 0;
    uint64_t orders_filled_ = 0;
    uint64_t orders_canceled_ = 0;
    uint64_t orders_failed_ = 0;
    double total_volume_ = 0.0;
    
    // PnL tracking
    mutable std::mutex pnl_mutex_;
    double current_position_ = 0.0;
    double previous_position_ = 0.0;
    double avg_buy_price_ = 0.0;
    double cumulative_pnl_ = 0.0;
    
    // Session statistics
    mutable std::mutex session_mutex_;
    std::chrono::system_clock::time_point session_start_time_;
    std::chrono::system_clock::time_point session_end_time_;
    double min_spread_observed_ = std::numeric_limits<double>::max();
    double max_spread_observed_ = std::numeric_limits<double>::lowest();
    uint64_t buy_trades_ = 0;
    uint64_t sell_trades_ = 0;
    double total_buy_volume_ = 0.0;
    double total_sell_volume_ = 0.0;
    double max_profit_per_trade_ = 0.0;
    double max_loss_per_trade_ = 0.0;
    uint64_t profitable_trades_ = 0;
    uint64_t losing_trades_ = 0;
    
    // Configuration
    int order_timeout_seconds_ = 30;
    int max_retry_attempts_ = 3;
    int rest_timeout_seconds_ = 5;
    
    // Health status
    mutable std::mutex health_mutex_;
    bool api_healthy_ = true;
    bool redis_healthy_ = true;
    std::chrono::system_clock::time_point last_health_check_;
    
    // Risk management
    RiskManager* risk_manager_ = nullptr;
    
    // Helper methods
    bool connectToRedis();
    void disconnectFromRedis();
    bool testRedisConnection();
    
    std::string generateClientOrderId();
    bool isOrderExpired(const Order& order) const;
    
    // REST API helpers (implemented in rest_client.cpp)
    OrderResponse executeOrder(const std::string& symbol, const std::string& side, 
                              double price, double quantity);
    OrderResponse executeCancelOrder(const std::string& symbol, const std::string& order_id);
    OrderStatusResponse executeOrderStatus(const std::string& symbol, const std::string& order_id);
    
    // Validation
    bool validateOrder(const std::string& symbol, const std::string& side, 
                      double price, double quantity) const;
    
    // Trade execution and logging
    void simulateOrderFill(Order& order);
    void logTrade(const Order& order);
    void updatePositionAndPnL(const Order& order);
    void logPnL(const Order& order, double realized_pnl);
    
    // Session statistics
    void initializeSession();
    void updateSessionStats(const Order& order, double spread_bps);
    void generateSessionSummary();
    
    // Latency tracking
    mutable std::mutex latency_mutex_;
    LatencyMetrics latency_metrics_;
    std::vector<double> order_latencies_;
    std::vector<double> fill_latencies_;
    std::vector<double> network_latencies_;
    bool latency_monitoring_enabled_ = false;
    std::chrono::high_resolution_clock::time_point last_latency_test_;
    
    // Latency helper methods
    void updateLatencyMetrics(double order_latency_ms, double fill_latency_ms = -1.0);
    void updateNetworkLatencyMetrics(double network_latency_ms);
}; 