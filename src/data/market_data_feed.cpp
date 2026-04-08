#include "data/market_data.h"
#include "data/websocket_client.h"
#include "metrics/metrics.h"
#include "core/spsc_queue.h"
#include "core/types.h"
#include <iostream>
#include <algorithm>

MarketDataFeed::MarketDataFeed(WebSocketClient& ws_client, AtomicHFTMetrics& metrics)
    : ws_client_(ws_client)
    , metrics_(metrics)
{
    last_ws_message_time_ = std::chrono::high_resolution_clock::now();
}

void MarketDataFeed::trimBook() {
    while (bid_book_.size() > MAX_BOOK_LEVELS) {
        bid_book_.erase(std::prev(bid_book_.end()));
    }
    while (ask_book_.size() > MAX_BOOK_LEVELS) {
        ask_book_.erase(std::prev(ask_book_.end()));
    }
}

void MarketDataFeed::start(const std::string& trading_symbol,
                           SPSCQueue<HFTMarketData, 1024>& queue) {
    trading_symbol_ = trading_symbol;

    ws_client_.setMessageCallback([this, &queue](const nlohmann::json& message) {
        try {
            if (HFT_UNLIKELY(!message.contains("channel") || message["channel"] != "l2_data" ||
                !message.contains("events") || !message["events"].is_array())) {
                return;
            }

            for (const auto& event : message["events"]) {
                if (!event.contains("type") || !event.contains("product_id") || !event.contains("updates")) {
                    continue;
                }

                const auto& product_id = event["product_id"].get_ref<const std::string&>();
                if (product_id != trading_symbol_) continue;

                const auto& type = event["type"].get_ref<const std::string&>();
                if (!event["updates"].is_array()) continue;

                bool is_snapshot = (type == "snapshot");
                if (!is_snapshot && type != "update") continue;

                if (is_snapshot) {
                    bid_book_.clear();
                    ask_book_.clear();
                }

                for (const auto& update : event["updates"]) {
                    if (!update.contains("side") || !update.contains("price_level") || !update.contains("new_quantity")) {
                        continue;
                    }

                    double price = std::stod(update["price_level"].get_ref<const std::string&>());
                    double qty = std::stod(update["new_quantity"].get_ref<const std::string&>());
                    const auto& side = update["side"].get_ref<const std::string&>();

                    if (side == "bid") {
                        if (qty > 0.0) {
                            bid_book_[price] = qty;
                        } else {
                            bid_book_.erase(price);
                        }
                    } else if (side == "offer") {
                        if (qty > 0.0) {
                            ask_book_[price] = qty;
                        } else {
                            ask_book_.erase(price);
                        }
                    }
                }

                trimBook();

                if (bid_book_.empty() || ask_book_.empty()) continue;

                double best_bid = bid_book_.begin()->first;
                double bid_qty = bid_book_.begin()->second;
                double best_ask = ask_book_.begin()->first;
                double ask_qty = ask_book_.begin()->second;

                if (HFT_UNLIKELY(best_bid >= best_ask)) continue;

                auto current_time = std::chrono::high_resolution_clock::now();
                auto processing_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    current_time - last_ws_message_time_).count();

                uint64_t display_latency_ns = static_cast<uint64_t>(
                    std::min(processing_time_ns, static_cast<int64_t>(50000000)));
                metrics_.websocket_latency_ns.store(display_latency_ns, std::memory_order_relaxed);
                last_ws_message_time_ = current_time;

                HFTMarketData market_data{};
                set_symbol(market_data.symbol, trading_symbol_);
                market_data.bid_price = best_bid;
                market_data.ask_price = best_ask;
                market_data.bid_quantity = bid_qty;
                market_data.ask_quantity = ask_qty;
                market_data.timestamp = current_time;
                market_data.sequence_number = ++sequence_counter_;

                current_bid_.store(best_bid, std::memory_order_relaxed);
                current_ask_.store(best_ask, std::memory_order_relaxed);
                double spread_bps = ((best_ask - best_bid) / best_bid) * 10000.0;
                current_spread_bps_.store(spread_bps, std::memory_order_relaxed);
                last_market_update_.store(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        current_time.time_since_epoch()).count()), std::memory_order_relaxed);

                queue.push(market_data);
                metrics_.market_data_updates.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (const std::exception& e) {
            std::cerr << "WebSocket message parsing error: " << e.what() << std::endl;
        }
    });
}
