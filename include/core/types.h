#pragma once

#include <string>
#include <chrono>
#include <array>
#include <cstring>
#include <algorithm>

enum class Side { BUY, SELL };

enum class OrderStatus { PENDING, NEW, FILLED, PARTIALLY_FILLED, CANCELED, REJECTED };

enum class OrderType { LIMIT, MARKET };

struct Order {
    std::string order_id;
    std::string symbol;
    Side side;
    OrderType type;
    double price;
    double quantity;
    double filled_quantity = 0.0;
    OrderStatus status = OrderStatus::PENDING;
    std::chrono::system_clock::time_point create_time;
    std::chrono::system_clock::time_point update_time;
    std::string client_order_id;
};

inline const char* to_string(Side s) {
    return s == Side::BUY ? "BUY" : "SELL";
}

inline const char* to_string(OrderStatus s) {
    switch (s) {
        case OrderStatus::PENDING:          return "PENDING";
        case OrderStatus::NEW:              return "NEW";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::CANCELED:         return "CANCELED";
        case OrderStatus::REJECTED:         return "REJECTED";
    }
    return "UNKNOWN";
}

inline void set_symbol(std::array<char, 16>& dest, const std::string& src) {
    size_t len = std::min(src.size(), dest.size() - 1);
    std::memcpy(dest.data(), src.data(), len);
    dest[len] = '\0';
}
