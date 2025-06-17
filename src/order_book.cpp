#include "order_book.h"
#include <iostream>

OrderBook::OrderBook(const std::string& symbol) : symbol_(symbol) {
    last_update_time_ = std::chrono::system_clock::now();
}

bool OrderBook::updateFromWebSocket(const nlohmann::json& depth_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        // Check if this is a Binance depth stream message
        if (!depth_data.contains("stream") || !depth_data.contains("data")) {
            return false;
        }
        
        const auto& data = depth_data["data"];
        
        // Verify it's a depth update
        if (!data.contains("bids") || !data.contains("asks")) {
            return false;
        }
        
        // Process bids
        if (data["bids"].is_array()) {
            std::vector<std::vector<std::string>> bids;
            for (const auto& bid : data["bids"]) {
                if (bid.is_array() && bid.size() >= 2) {
                    bids.push_back({bid[0].get<std::string>(), bid[1].get<std::string>()});
                }
            }

            updateBids(bids);
        }
        
        // Process asks
        if (data["asks"].is_array()) {
            std::vector<std::vector<std::string>> asks;
            for (const auto& ask : data["asks"]) {
                if (ask.is_array() && ask.size() >= 2) {
                    asks.push_back({ask[0].get<std::string>(), ask[1].get<std::string>()});
                }
            }

            updateAsks(asks);
        }
        
        last_update_time_ = std::chrono::system_clock::now();
        update_count_++;
        invalidateCache();
        
        // Market data successfully processed - silent operation in production
        
        return true;
        
    } catch (const std::exception& e) {
        std::cout << "Error processing WebSocket depth data: " << e.what() << std::endl;
        return false;
    }
}

bool OrderBook::updateBids(const std::vector<std::vector<std::string>>& bids) {
    // Note: mutex already held by caller
    
    for (const auto& level : bids) {
        double price, quantity;
        if (parseDepthLevel(level, price, quantity)) {
            if (quantity == 0.0) {
                // Remove this price level
                bids_.erase(price);
            } else {
                // Update price level
                bids_[price] = quantity;
            }
        }
    }
    
    // Clean up any zero quantity levels
    cleanupLevels();
    
    return true;
}

bool OrderBook::updateAsks(const std::vector<std::vector<std::string>>& asks) {
    // Note: mutex already held by caller
    
    for (const auto& level : asks) {
        double price, quantity;
        if (parseDepthLevel(level, price, quantity)) {
            if (quantity == 0.0) {
                // Remove this price level
                asks_.erase(price);
            } else {
                // Update price level
                asks_[price] = quantity;
            }
        }
    }
    
    // Clean up any zero quantity levels
    cleanupLevels();
    
    return true;
}

OrderBookSnapshot OrderBook::getSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    OrderBookSnapshot snapshot;
    snapshot.symbol = symbol_;
    snapshot.timestamp = last_update_time_;
    
    // Fill in bid/ask data directly (avoid deadlock with getBestBidPrice() etc.)
    if (!bids_.empty()) {
        snapshot.best_bid_price = bids_.begin()->first;
        snapshot.best_bid_quantity = bids_.begin()->second;
    }
    
    if (!asks_.empty()) {
        snapshot.best_ask_price = asks_.begin()->first;
        snapshot.best_ask_quantity = asks_.begin()->second;
    }
    
    // Calculate spread
    if (!bids_.empty() && !asks_.empty()) {
        snapshot.spread = snapshot.best_ask_price - snapshot.best_bid_price;
        double mid = (snapshot.best_bid_price + snapshot.best_ask_price) / 2.0;
        snapshot.spread_bps = mid > 0 ? (snapshot.spread / mid) * 10000 : 0.0;
    }
    
    // Get top levels for the snapshot
    size_t count = 0;
    for (const auto& [price, quantity] : bids_) {
        if (count >= 10) break;
        snapshot.bids.emplace_back(price, quantity);
        count++;
    }
    
    count = 0;
    for (const auto& [price, quantity] : asks_) {
        if (count >= 10) break;
        snapshot.asks.emplace_back(price, quantity);
        count++;
    }
    
    // Mark as valid if we have both bids and asks
    snapshot.is_valid = !bids_.empty() && !asks_.empty();
    
    return snapshot;
}

double OrderBook::getBestBidPrice() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bids_.empty()) {
        return 0.0;
    }
    // bids_ is sorted in descending order, so first element is highest price
    return bids_.begin()->first;
}

double OrderBook::getBestAskPrice() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (asks_.empty()) {
        return 0.0;
    }
    // asks_ is sorted in ascending order, so first element is lowest price
    return asks_.begin()->first;
}

double OrderBook::getBestBidQuantity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bids_.empty()) {
        return 0.0;
    }
    return bids_.begin()->second;
}

double OrderBook::getBestAskQuantity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (asks_.empty()) {
        return 0.0;
    }
    return asks_.begin()->second;
}

double OrderBook::getSpread() const {
    return getBestAskPrice() - getBestBidPrice();
}

double OrderBook::getSpreadBps() const {
    double spread = getSpread();
    double mid = getMidPrice();
    return mid > 0 ? (spread / mid) * 10000 : 0.0;
}

double OrderBook::getMidPrice() const {
    return (getBestBidPrice() + getBestAskPrice()) / 2.0;
}

bool OrderBook::isValid() const {
    return true; // Stub
}

std::chrono::system_clock::time_point OrderBook::getLastUpdateTime() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_update_time_;
}

size_t OrderBook::getBidLevels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bids_.size();
}

size_t OrderBook::getAskLevels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return asks_.size();
}

std::vector<OrderBookLevel> OrderBook::getBids(size_t max_levels) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OrderBookLevel> result;
    
    size_t count = 0;
    for (const auto& [price, quantity] : bids_) {
        if (count >= max_levels) break;
        result.emplace_back(price, quantity);
        count++;
    }
    
    return result;
}

std::vector<OrderBookLevel> OrderBook::getAsks(size_t max_levels) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OrderBookLevel> result;
    
    size_t count = 0;
    for (const auto& [price, quantity] : asks_) {
        if (count >= max_levels) break;
        result.emplace_back(price, quantity);
        count++;
    }
    
    return result;
}

double OrderBook::getBidLiquidity(double price_threshold) const {
    return 0.0; // Stub
}

double OrderBook::getAskLiquidity(double price_threshold) const {
    return 0.0; // Stub
}

void OrderBook::printStats() const {
    std::cout << "OrderBook Stats - Symbol: " << symbol_ 
              << " Updates: " << update_count_ << std::endl;
}

void OrderBook::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    bids_.clear();
    asks_.clear();
    invalidateCache();
}

void OrderBook::updateCache() const {
    // Stub
    cache_valid_ = true;
}

void OrderBook::invalidateCache() {
    cache_valid_ = false;
}

bool OrderBook::parseDepthLevel(const std::vector<std::string>& level, double& price, double& quantity) const {
    if (level.size() < 2) {
        return false;
    }
    
    try {
        price = std::stod(level[0]);
        quantity = std::stod(level[1]);
        
        // Basic validation
        if (price <= 0.0 || quantity < 0.0) {
            return false;
        }
        
        return true;
        
    } catch (const std::exception&) {
        return false;
    }
}

void OrderBook::cleanupLevels() {
    // Remove zero quantity levels (already done in update logic)
    // But we can also limit the number of levels to save memory
    
    const size_t MAX_LEVELS = 100;
    
    // Cleanup bids (keep top MAX_LEVELS highest prices)
    if (bids_.size() > MAX_LEVELS) {
        auto it = bids_.begin();
        std::advance(it, MAX_LEVELS);
        bids_.erase(it, bids_.end());
    }
    
    // Cleanup asks (keep top MAX_LEVELS lowest prices)
    if (asks_.size() > MAX_LEVELS) {
        auto it = asks_.begin();
        std::advance(it, MAX_LEVELS);
        asks_.erase(it, asks_.end());
    }
}

bool OrderBook::validateOrderBook() const {
    // Stub
    return true;
} 