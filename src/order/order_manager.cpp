#include "order/order_manager.h"
#include <iostream>
#include <random>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <ctime>

OrderManager::OrderManager() {
    initializeSession();
}

OrderManager::~OrderManager() {
    shutdown();
}

bool OrderManager::initialize() {
    std::cout << "Order Manager initialized" << std::endl;
    return true;
}

void OrderManager::shutdown() {
    if (shutdown_called_) return;
    shutdown_called_ = true;

    generateSessionSummary();
    std::cout << "Order Manager shutdown complete" << std::endl;
}

OrderResponse OrderManager::placeOrder(const std::string& symbol, Side side, double price, double quantity) {
    return executeOrder(symbol, side, price, quantity);
}

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

std::string OrderManager::generateClientOrderId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(100000, 999999);

    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    return "HFT_" + std::to_string(timestamp) + "_" + std::to_string(dis(gen));
}

OrderResponse OrderManager::executeOrder(const std::string& symbol, Side side, double price, double quantity) {
    OrderResponse response;

    if (!validateOrder(symbol, side, price, quantity)) {
        response.success = false;
        response.error_message = "Invalid order parameters";
        return response;
    }

    std::string client_order_id = generateClientOrderId();

    Order order;
    order.order_id = client_order_id;
    order.client_order_id = client_order_id;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::LIMIT;
    order.quantity = quantity;
    order.price = price;
    order.status = OrderStatus::NEW;
    order.create_time = std::chrono::system_clock::now();
    order.update_time = order.create_time;

    orders_placed_++;

    simulateOrderFill(order);

    response.success = true;
    response.order_id = client_order_id;
    response.status = "FILLED";
    response.filled_quantity = quantity;
    response.avg_fill_price = price;

    return response;
}

bool OrderManager::validateOrder(const std::string& symbol, Side /*side*/, double price, double quantity) const {
    if (symbol.empty()) return false;
    if (price <= 0.0) return false;
    if (quantity <= 0.0) return false;
    if (quantity < 0.001) return false;
    if (quantity > 10.0) return false;
    if (price < 100.0 || price > 10000.0) return false;

    return true;
}

void OrderManager::simulateOrderFill(Order& order) {
    order.status = OrderStatus::FILLED;
    order.filled_quantity = order.quantity;
    order.update_time = std::chrono::system_clock::now();

    orders_filled_++;
    updatePositionAndPnL(order);
    updateSessionStats(order);
}

void OrderManager::updatePositionAndPnL(const Order& order) {
    std::lock_guard<std::mutex> lock(pnl_mutex_);

    double old_position = current_position_;
    double fill_qty = order.filled_quantity;
    double fill_price = order.price;
    bool is_buy = (order.side == Side::BUY);

    bool is_closing = (is_buy && old_position < 0) || (!is_buy && old_position > 0);

    double realized_pnl = 0.0;

    if (is_closing) {
        double closing_qty = std::min(fill_qty, std::abs(old_position));
        if (old_position > 0) {
            realized_pnl = (fill_price - avg_entry_price_) * closing_qty;
        } else {
            realized_pnl = (avg_entry_price_ - fill_price) * closing_qty;
        }

        double remaining_qty = fill_qty - closing_qty;
        current_position_ = is_buy ? remaining_qty : -remaining_qty;

        if (remaining_qty > 1e-12) {
            avg_entry_price_ = fill_price;
        } else if (std::abs(current_position_) < 1e-12) {
            avg_entry_price_ = 0.0;
        }
    } else {
        double abs_old = std::abs(old_position);
        double abs_new = abs_old + fill_qty;
        if (abs_new > 1e-12) {
            avg_entry_price_ = (avg_entry_price_ * abs_old + fill_price * fill_qty) / abs_new;
        } else {
            avg_entry_price_ = fill_price;
        }
        current_position_ += is_buy ? fill_qty : -fill_qty;
    }

    cumulative_pnl_ += realized_pnl;
}

void OrderManager::initializeSession() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    session_start_time_ = std::chrono::system_clock::now();
    buy_trades_ = 0;
    sell_trades_ = 0;
    total_buy_volume_ = 0.0;
    total_sell_volume_ = 0.0;
}

void OrderManager::updateSessionStats(const Order& order) {
    std::lock_guard<std::mutex> lock(session_mutex_);

    if (order.side == Side::BUY) {
        buy_trades_++;
        total_buy_volume_ += order.filled_quantity;
    } else {
        sell_trades_++;
        total_sell_volume_ += order.filled_quantity;
    }
}

void OrderManager::generateSessionSummary() {
    double final_pnl;
    double final_position;
    double avg_price;
    {
        std::lock_guard<std::mutex> lock(pnl_mutex_);
        final_pnl = cumulative_pnl_;
        final_position = current_position_;
        avg_price = avg_entry_price_;
    }

    std::lock_guard<std::mutex> lock(session_mutex_);
    session_end_time_ = std::chrono::system_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        session_end_time_ - session_start_time_);
    double duration_seconds = static_cast<double>(duration.count());

    uint64_t total_trades = buy_trades_ + sell_trades_;
    double trade_rate = (duration_seconds > 0) ? (total_trades / duration_seconds) : 0.0;
    double total_volume = total_buy_volume_ + total_sell_volume_;

    auto start_time_t = std::chrono::system_clock::to_time_t(session_start_time_);
    auto end_time_t = std::chrono::system_clock::to_time_t(session_end_time_);

    struct tm start_tm{};
    struct tm end_tm{};
    localtime_r(&start_time_t, &start_tm);
    localtime_r(&end_time_t, &end_tm);

    std::ofstream summary_file("logs/session_summary.log", std::ios::app);
    if (!summary_file.is_open()) return;

    uint64_t placed = orders_placed_.load();
    uint64_t filled = orders_filled_.load();

    summary_file << "\n" << std::string(80, '=') << std::endl;
    summary_file << "                    HFT TRADING SESSION SUMMARY" << std::endl;
    summary_file << std::string(80, '=') << std::endl;

    summary_file << std::put_time(&start_tm, "Session Start: %Y-%m-%d %H:%M:%S") << std::endl;
    summary_file << std::put_time(&end_tm, "Session End:   %Y-%m-%d %H:%M:%S") << std::endl;
    summary_file << "Duration: " << duration_seconds << " seconds ("
                 << std::fixed << std::setprecision(2) << duration_seconds / 60.0 << " minutes)" << std::endl;

    summary_file << "\nTRADING PERFORMANCE:" << std::endl;
    summary_file << "  Total Trades: " << total_trades << std::endl;
    summary_file << "  BUY Trades:   " << buy_trades_ << std::endl;
    summary_file << "  SELL Trades:  " << sell_trades_ << std::endl;
    summary_file << "  Trade Rate:   " << std::fixed << std::setprecision(2) << trade_rate << " trades/sec" << std::endl;
    summary_file << "  Total Volume: " << std::fixed << std::setprecision(8) << total_volume << " ETH" << std::endl;
    summary_file << "  Buy Volume:   " << std::fixed << std::setprecision(8) << total_buy_volume_ << " ETH" << std::endl;
    summary_file << "  Sell Volume:  " << std::fixed << std::setprecision(8) << total_sell_volume_ << " ETH" << std::endl;

    summary_file << "\nPROFIT & LOSS:" << std::endl;
    summary_file << "  Final Position:   " << std::fixed << std::setprecision(8) << final_position << " ETH" << std::endl;
    summary_file << "  Cumulative PnL:   $" << std::fixed << std::setprecision(4) << final_pnl << std::endl;
    summary_file << "  Avg Entry Price:  $" << std::fixed << std::setprecision(2) << avg_price << std::endl;
    if (total_trades > 0) {
        summary_file << "  PnL per Trade:    $" << std::fixed << std::setprecision(6) << (final_pnl / total_trades) << std::endl;
    }

    summary_file << "\nSYSTEM STATS:" << std::endl;
    summary_file << "  Orders Placed: " << placed << std::endl;
    summary_file << "  Orders Filled: " << filled << std::endl;
    if (placed > 0) {
        summary_file << "  Fill Rate:     " << std::fixed << std::setprecision(1)
                     << (filled * 100.0 / placed) << "%" << std::endl;
    }

    if (buy_trades_ > 0 && sell_trades_ > 0) {
        summary_file << "\nMARKET MAKING:" << std::endl;
        summary_file << "  Trade Balance: " << std::fixed << std::setprecision(1)
                     << (std::min(buy_trades_, sell_trades_) * 100.0 / std::max(buy_trades_, sell_trades_)) << "% balanced" << std::endl;
    }
    if (total_volume > 0 && duration_seconds > 0) {
        summary_file << "  Turnover Rate: " << std::fixed << std::setprecision(2)
                     << (total_volume / duration_seconds) << " ETH/sec" << std::endl;
    }

    summary_file << std::string(80, '=') << std::endl;
    summary_file.close();

    std::cout << "\nSession Summary Generated: logs/session_summary.log" << std::endl;
    std::cout << "Total Trades: " << total_trades << " | PnL: $" << std::fixed << std::setprecision(4) << final_pnl
              << " | Duration: " << duration_seconds << "s" << std::endl;
}
