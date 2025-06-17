#pragma once

#include <map>
#include <string>
#include <mutex>
#include <vector>
#include <chrono>
#include <nlohmann/json.hpp>

struct OrderBookLevel {
    double price;
    double quantity;
    
    OrderBookLevel(double p = 0.0, double q = 0.0) : price(p), quantity(q) {}
};

struct OrderBookSnapshot {
    std::string symbol;
    std::chrono::system_clock::time_point timestamp;
    std::vector<OrderBookLevel> bids;
    std::vector<OrderBookLevel> asks;
    double best_bid_price = 0.0;
    double best_ask_price = 0.0;
    double best_bid_quantity = 0.0;
    double best_ask_quantity = 0.0;
    double spread = 0.0;
    double spread_bps = 0.0;
    bool is_valid = false;
};

class OrderBook {
public:
    explicit OrderBook(const std::string& symbol);
    ~OrderBook() = default;
    
    // Update methods
    bool updateFromWebSocket(const nlohmann::json& depth_data);
    bool updateBids(const std::vector<std::vector<std::string>>& bids);
    bool updateAsks(const std::vector<std::vector<std::string>>& asks);
    
    // Getters
    OrderBookSnapshot getSnapshot() const;
    double getBestBidPrice() const;
    double getBestAskPrice() const;
    double getBestBidQuantity() const;
    double getBestAskQuantity() const;
    double getSpread() const;
    double getSpreadBps() const;
    double getMidPrice() const;
    
    // Utility methods
    bool isValid() const;
    std::chrono::system_clock::time_point getLastUpdateTime() const;
    size_t getBidLevels() const;
    size_t getAskLevels() const;
    
    // Get depth at specific levels
    std::vector<OrderBookLevel> getBids(size_t max_levels = 10) const;
    std::vector<OrderBookLevel> getAsks(size_t max_levels = 10) const;
    
    // Calculate liquidity
    double getBidLiquidity(double price_threshold) const;
    double getAskLiquidity(double price_threshold) const;
    
    // Statistics
    void printStats() const;
    void reset();

private:
    std::string symbol_;
    mutable std::mutex mutex_;
    
    // Order book data - using maps for automatic sorting
    std::map<double, double, std::greater<double>> bids_;  // price -> quantity (descending)
    std::map<double, double> asks_;  // price -> quantity (ascending)
    
    // Cached values for performance
    mutable double cached_best_bid_ = 0.0;
    mutable double cached_best_ask_ = 0.0;
    mutable double cached_spread_ = 0.0;
    mutable bool cache_valid_ = false;
    
    // Metadata
    std::chrono::system_clock::time_point last_update_time_;
    uint64_t update_count_ = 0;
    
    // Helper methods
    void updateCache() const;
    void invalidateCache();
    bool parseDepthLevel(const std::vector<std::string>& level, double& price, double& quantity) const;
    void cleanupLevels();  // Remove zero quantity levels
    
    // Validation
    bool validateOrderBook() const;
}; 