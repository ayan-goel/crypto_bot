#include "strategy.h"
#include <iostream>

Strategy::Strategy(const std::string& symbol) : symbol_(symbol) {
    day_start_time_ = std::chrono::system_clock::now();
}

StrategySignal Strategy::generateSignal(const OrderBookSnapshot& orderbook) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    StrategySignal signal;
    signals_generated_++;
    
    // Check if orderbook is valid
    if (!orderbook.is_valid) {
        signal.reason = "Invalid orderbook data";
        return signal;
    }
    
    // For HFT, always try to place orders regardless of spread
    // Remove spread threshold check to be more aggressive
    // if (orderbook.spread_bps <= spread_threshold_bps_) {
    //     signal.reason = "Spread too narrow (" + std::to_string(orderbook.spread_bps) + " bps < " + std::to_string(spread_threshold_bps_) + " bps)";
    //     return signal;
    // }
    
    // For HFT, be very aggressive - only check basic risk limits
    if (circuit_breaker_triggered_) {
        signal.reason = "Circuit breaker triggered";
        return signal;
    }
    
    // Calculate optimal bid/ask prices with offsets
    double optimal_bid = calculateOptimalBidPrice(orderbook);
    double optimal_ask = calculateOptimalAskPrice(orderbook);
    
    // Calculate order quantities based on inventory
    double bid_quantity = calculateOrderQuantity("BUY");
    double ask_quantity = calculateOrderQuantity("SELL");
    
    // For HFT, always place orders on both sides unless severely constrained
    bool can_place_bid = isInventoryWithinLimits(bid_quantity);
    bool can_place_ask = isInventoryWithinLimits(-ask_quantity);
    
    // Always try to place both sides - be very aggressive
    signal.should_place_bid = can_place_bid;
    signal.should_place_ask = can_place_ask;
    signal.bid_price = optimal_bid;
    signal.ask_price = optimal_ask;
    signal.bid_quantity = bid_quantity;
    signal.ask_quantity = ask_quantity;
    
    if (signal.should_place_bid || signal.should_place_ask) {
        signal.reason = "HFT market making - spread " + std::to_string(orderbook.spread_bps) + " bps";
    } else {
        signal.reason = "Inventory limits prevent orders";
    }
    
    return signal;
}

void Strategy::updatePosition(const Order& filled_order) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Stub - just increment filled count
    orders_filled_++;
}

Position Strategy::getCurrentPosition() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_position_;
}

double Strategy::getUnrealizedPnL(double current_price) const {
    return 0.0; // Stub
}

double Strategy::getTotalPnL(double current_price) const {
    return 0.0; // Stub
}

bool Strategy::isWithinRiskLimits(const StrategySignal& signal) const {
    return !circuit_breaker_triggered_; // Stub
}

bool Strategy::isInventoryWithinLimits(double additional_quantity) const {
    double new_position = current_position_.quantity + additional_quantity;
    
    // For HFT, be much more permissive with inventory limits
    // Only block if we would severely exceed limits
    if (std::abs(new_position) > max_inventory_ * 2.0) {
        return false;
    }
    
    return true;
}

bool Strategy::isDailyDrawdownExceeded() const {
    // Calculate current day's PnL
    double current_pnl = getTotalPnL(0.0);  // Would need current market price
    
    // Check if daily loss exceeds maximum allowed drawdown
    if (current_pnl < -max_daily_drawdown_) {
        return true;
    }
    
    return false;
}

void Strategy::addPendingOrder(const Order& order) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_orders_.push_back(order);
}

void Strategy::updatePendingOrder(const Order& order) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Stub - find and update order
}

void Strategy::removePendingOrder(const std::string& order_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Stub - remove order
}

std::vector<Order> Strategy::getPendingOrders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_orders_;
}

void Strategy::setSpreadThreshold(double bps) {
    spread_threshold_bps_ = bps;
}

void Strategy::setOrderSize(double size) {
    order_size_ = size;
}

void Strategy::setMaxInventory(double max_inventory) {
    max_inventory_ = max_inventory;
}

void Strategy::setOrderOffset(double bid_offset_bps, double ask_offset_bps) {
    bid_offset_bps_ = bid_offset_bps;
    ask_offset_bps_ = ask_offset_bps;
}

void Strategy::updateStats(const OrderBookSnapshot& orderbook) {
    // Stub
}

void Strategy::printStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << "Strategy Stats - Symbol: " << symbol_ 
              << " Signals: " << signals_generated_
              << " Filled: " << orders_filled_ << std::endl;
}

void Strategy::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_orders_.clear();
    signals_generated_ = 0;
    orders_filled_ = 0;
}

void Strategy::enableCircuitBreaker(bool enabled) {
    circuit_breaker_enabled_ = enabled;
}

bool Strategy::isCircuitBreakerTriggered() const {
    return circuit_breaker_triggered_;
}

double Strategy::calculateOptimalBidPrice(const OrderBookSnapshot& orderbook) const {
    // For HFT, place bid ABOVE best bid to get ahead of queue
    double offset = orderbook.best_bid_price * (bid_offset_bps_ / 10000.0);
    return orderbook.best_bid_price + offset;
}

double Strategy::calculateOptimalAskPrice(const OrderBookSnapshot& orderbook) const {
    // For HFT, place ask BELOW best ask to get ahead of queue
    double offset = orderbook.best_ask_price * (ask_offset_bps_ / 10000.0);
    return orderbook.best_ask_price - offset;
}

double Strategy::calculateOrderQuantity(const std::string& side) const {
    // Use configured order size
    double quantity = order_size_;
    
    // Don't reduce quantity much - we want maximum trading
    double current_inventory = current_position_.quantity;
    
    if (side == "BUY" && current_inventory > max_inventory_ * 1.5) {
        quantity *= 0.5;  // Only reduce when way over limit
    } else if (side == "SELL" && current_inventory < -max_inventory_ * 1.5) {
        quantity *= 0.5;  // Only reduce when way over limit
    }
    
    return quantity;
}

bool Strategy::shouldPlaceNewOrders(const OrderBookSnapshot& orderbook) const {
    // Check circuit breaker
    if (circuit_breaker_triggered_) {
        return false;
    }
    
    // Check daily drawdown limits
    if (isDailyDrawdownExceeded()) {
        return false;
    }
    
    // Don't place orders if we have too many pending already - increased for HFT
    if (pending_orders_.size() >= 50) {
        return false;
    }
    
    // For HFT, we want to trade very frequently - remove recent order check
    // if (hasRecentOrders()) {
    //     return false;
    // }
    
    return true;
}

bool Strategy::hasRecentOrders() const {
    return false; // Stub
}

void Strategy::updateDailyTracking() {
    // Stub
}

bool Strategy::validateSignal(const StrategySignal& signal, const OrderBookSnapshot& orderbook) const {
    return true; // Stub
} 