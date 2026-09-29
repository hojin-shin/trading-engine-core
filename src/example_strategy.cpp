#include "trading/strategy.hpp"

#include <utility>

namespace trading {

ExampleStrategy::ExampleStrategy(std::string symbol, Quantity quantity)
    : symbol_(std::move(symbol)), quantity_(quantity) {
    validate_signal({symbol_, Side::Buy, quantity_, OrderType::Market, std::nullopt});
}

std::optional<Signal> ExampleStrategy::on_market_data(const MarketData& data) {
    if (data.symbol != symbol_ || ticks_ >= 3) { return std::nullopt; }
    ++ticks_;
    if (ticks_ == 1 || ticks_ == 3) {
        return Signal{symbol_, ticks_ == 1 ? Side::Buy : Side::Sell,
                      quantity_, OrderType::Market, std::nullopt};
    }
    return std::nullopt;
}

} // namespace trading
