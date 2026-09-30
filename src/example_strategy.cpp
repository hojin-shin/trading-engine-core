#include "trading/strategy.hpp"

#include <utility>

namespace trading {

ExampleStrategy::ExampleStrategy(std::string symbol, Quantity quantity,
                                 std::optional<ExampleLimitPrices> limits)
    : symbol_(std::move(symbol)), quantity_(quantity), limits_(limits) {
    if (limits_) {
        validate_signal({symbol_, Side::Buy, quantity_, OrderType::Limit, limits_->buy});
        validate_signal({symbol_, Side::Sell, quantity_, OrderType::Limit, limits_->sell});
    } else {
        validate_signal({symbol_, Side::Buy, quantity_, OrderType::Market, std::nullopt});
    }
}

std::optional<Signal> ExampleStrategy::on_market_data(const MarketData& data) {
    if (data.symbol != symbol_ || ticks_ >= 3) { return std::nullopt; }
    ++ticks_;
    if (ticks_ == 1 || ticks_ == 3) {
        const auto side = ticks_ == 1 ? Side::Buy : Side::Sell;
        if (limits_) {
            return Signal{symbol_, side, quantity_, OrderType::Limit,
                          side == Side::Buy ? limits_->buy : limits_->sell};
        }
        return Signal{symbol_, side, quantity_, OrderType::Market, std::nullopt};
    }
    return std::nullopt;
}

} // namespace trading
