#pragma once

#include "trading/types.hpp"

#include <optional>
#include <string>

namespace trading {

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual std::optional<Signal> on_market_data(const MarketData& data) = 0;
};

struct ExampleLimitPrices {
    double buy{};
    double sell{};
};

// A scripted demonstration: buy on the first matching tick, sell on the third.
class ExampleStrategy final : public IStrategy {
public:
    explicit ExampleStrategy(std::string symbol = "SYNTH", Quantity quantity = 2,
                             std::optional<ExampleLimitPrices> limits = std::nullopt);
    std::optional<Signal> on_market_data(const MarketData& data) override;

private:
    std::string symbol_;
    Quantity quantity_;
    std::optional<ExampleLimitPrices> limits_;
    unsigned int ticks_{};
};

} // namespace trading
