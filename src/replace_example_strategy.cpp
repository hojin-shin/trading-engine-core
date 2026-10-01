#include "trading/strategy.hpp"

#include <utility>

namespace trading {

ReplaceExampleStrategy::ReplaceExampleStrategy(std::string symbol, Quantity quantity,
                                               double initial_price, double replacement_price)
    : symbol_(std::move(symbol)), quantity_(quantity), initial_price_(initial_price),
      replacement_price_(replacement_price) {
    validate_signal({symbol_, Side::Buy, quantity_, OrderType::Limit, initial_price_});
    validate_signal({symbol_, Side::Buy, quantity_, OrderType::Limit, replacement_price_});
}

std::optional<StrategyAction> ReplaceExampleStrategy::on_market_data(const MarketData& data) {
    if (data.symbol != symbol_ || ticks_ >= 2) { return std::nullopt; }
    ++ticks_;
    if (ticks_ == 1) {
        return Signal{symbol_, Side::Buy, quantity_, OrderType::Limit, initial_price_};
    }
    if (order_id_) { return ReplaceRequest{*order_id_, replacement_price_}; }
    return std::nullopt;
}

void ReplaceExampleStrategy::on_order_created(const Order& order) {
    if (!order_id_) { order_id_ = order.id; }
}

} // namespace trading
