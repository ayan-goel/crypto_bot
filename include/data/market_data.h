#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <map>
#include <functional>
#include <string>

struct AtomicHFTMetrics;
class WebSocketClient;

template<typename T, size_t Size>
class SPSCQueue;

struct HFTMarketData {
    std::array<char, 16> symbol{};
    double bid_price = 0.0;
    double ask_price = 0.0;
    double bid_quantity = 0.0;
    double ask_quantity = 0.0;
    std::chrono::high_resolution_clock::time_point timestamp;
    uint64_t sequence_number = 0;
};

class MarketDataFeed {
public:
    MarketDataFeed(WebSocketClient& ws_client, AtomicHFTMetrics& metrics);

    void start(const std::string& trading_symbol,
               SPSCQueue<HFTMarketData, 1024>& queue);

    double bid() const { return current_bid_.load(); }
    double ask() const { return current_ask_.load(); }
    double spread_bps() const { return current_spread_bps_.load(); }

private:
    WebSocketClient& ws_client_;
    AtomicHFTMetrics& metrics_;
    std::string trading_symbol_;

    std::map<double, double, std::greater<>> bid_book_;
    std::map<double, double> ask_book_;

    std::atomic<double> current_bid_{0.0};
    std::atomic<double> current_ask_{0.0};
    std::atomic<double> current_spread_bps_{0.0};
    std::atomic<uint64_t> last_market_update_{0};
    std::chrono::high_resolution_clock::time_point last_ws_message_time_;
    std::atomic<uint64_t> sequence_counter_{0};

    static constexpr size_t MAX_BOOK_LEVELS = 25;
    void trimBook();
};
