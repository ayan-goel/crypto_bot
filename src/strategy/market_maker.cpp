#include "strategy/market_maker.h"
#include "core/config.h"
#include <cmath>
#include <algorithm>

MarketMakingStrategy::MarketMakingStrategy() {
    Config& config = Config::getInstance();
    tick_size_ = config.getTickSize();
    spread_offset_ = tick_size_ * config.getSpreadOffsetTicks();
    min_spread_ = tick_size_ * config.getMinSpreadTicks();
    max_neutral_pos_ = config.getMaxNeutralPosition();
    inventory_ceiling_ = config.getInventoryCeiling();
    num_levels_ = static_cast<uint32_t>(config.getOrderLadderLevels());
}

HFTSignal MarketMakingStrategy::generate_signal(double bid, double ask,
                                                 double current_position,
                                                 double order_size) const {
    HFTSignal signal{};

    signal.place_bid = true;
    signal.place_ask = true;
    signal.num_levels = num_levels_;

    signal.bid_price = bid - spread_offset_;
    signal.ask_price = ask + spread_offset_;

    if ((signal.ask_price - signal.bid_price) < min_spread_) {
        double mid = (bid + ask) / 2.0;
        signal.bid_price = mid - (min_spread_ / 2.0);
        signal.ask_price = mid + (min_spread_ / 2.0);
    }

    signal.bid_quantity = order_size;
    signal.ask_quantity = order_size;

    if (std::abs(current_position) > max_neutral_pos_) {
        if (current_position > max_neutral_pos_) {
            signal.bid_quantity *= 0.5;
            signal.ask_quantity *= 1.5;
            signal.ask_price = ask + (tick_size_ * 1.5);
        } else {
            signal.ask_quantity *= 0.5;
            signal.bid_quantity *= 1.5;
            signal.bid_price = bid - (tick_size_ * 1.5);
        }
    }

    double inventory_penalty = std::min(0.8, std::abs(current_position) / inventory_ceiling_);
    if (inventory_penalty > 0.2) {
        signal.bid_quantity *= (1.0 - inventory_penalty);
        signal.ask_quantity *= (1.0 - inventory_penalty);
    }

    return signal;
}
