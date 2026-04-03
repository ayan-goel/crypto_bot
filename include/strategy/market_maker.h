#pragma once

#include <cstdint>

struct HFTSignal {
    bool place_bid = false;
    bool place_ask = false;
    bool cancel_orders = false;
    double bid_price = 0.0;
    double ask_price = 0.0;
    double bid_quantity = 0.0;
    double ask_quantity = 0.0;
    uint32_t num_levels = 0;
};

class MarketMakingStrategy {
public:
    MarketMakingStrategy();

    HFTSignal generate_signal(double bid, double ask,
                              double current_position, double order_size) const;

private:
    double tick_size_;
    double spread_offset_;
    double min_spread_;
    double max_neutral_pos_;
    double inventory_ceiling_;
    uint32_t num_levels_;
};
