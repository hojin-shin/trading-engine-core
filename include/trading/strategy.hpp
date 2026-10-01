#pragma once

#include "trading/types.hpp"

#include <optional>
#include <string>
#include <variant>

namespace trading {

using StrategyAction = std::variant<Signal, CancelRequest>;

class IStrategy {
public:
    virtual ~IStrategy() = default;
    // Existing orders match the tick before the strategy chooses one action.
    virtual std::optional<StrategyAction> on_market_data(const MarketData& data) = 0;
    // Called once after a new order's initial risk/submission/fills are processed.
    // Includes risk-rejected orders. Copy needed fields; do not retain the reference.
    virtual void on_order_created(const Order&) {}
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
    std::optional<StrategyAction> on_market_data(const MarketData& data) override;

private:
    std::string symbol_;
    Quantity quantity_;
    std::optional<ExampleLimitPrices> limits_;
    unsigned int ticks_{};
};

// Submit one buy limit on matching tick 1; request its cancellation on tick 2.
class CancelExampleStrategy final : public IStrategy {
public:
    explicit CancelExampleStrategy(std::string symbol = "SYNTH", Quantity quantity = 2,
                                   double limit_price = 100.0);
    std::optional<StrategyAction> on_market_data(const MarketData& data) override;
    void on_order_created(const Order& order) override;

private:
    std::string symbol_;
    Quantity quantity_;
    double limit_price_;
    unsigned int ticks_{};
    std::optional<OrderId> order_id_;
};

} // namespace trading
