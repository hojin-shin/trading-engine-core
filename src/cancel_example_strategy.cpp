#include "trading/strategy.hpp"

#include <utility>

namespace trading {

CancelExampleStrategy::CancelExampleStrategy(std::string symbol, Quantity quantity, double limit_price)
    : symbol_(std::move(symbol)), quantity_(quantity), limit_price_(limit_price) {
    validate_signal({symbol_, Side::Buy, quantity_, OrderType::Limit, limit_price_});
}

std::optional<StrategyAction> CancelExampleStrategy::on_market_data(const MarketData& data) {
    if (data.symbol != symbol_ || ticks_ >= 2) { return std::nullopt; }
    ++ticks_;
    if (ticks_ == 1) {
        return Signal{symbol_, Side::Buy, quantity_, OrderType::Limit, limit_price_};
    }
    if (order_id_) { return CancelRequest{*order_id_}; }
    return std::nullopt;
}

void CancelExampleStrategy::on_order_created(const Order& order) {
    order_id_ = order.id;
}

} // namespace trading
