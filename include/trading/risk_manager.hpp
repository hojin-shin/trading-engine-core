#pragma once

#include "trading/order_manager.hpp"
#include "trading/position_manager.hpp"

#include <string>

namespace trading {

struct RiskLimits {
    Quantity max_order_quantity{10};
    Quantity max_absolute_position{20};
    double max_order_notional{100'000.0};
    double max_loss{1'000.0};
};

struct RiskDecision {
    bool approved{};
    std::string reason;
};

class RiskManager {
public:
    explicit RiskManager(RiskLimits limits = {});
    [[nodiscard]] RiskDecision check(const Signal& signal, const MarketData& quote,
                                     const PositionManager& positions,
                                     const OrderManager& orders) const;

private:
    RiskLimits limits_;
};

} // namespace trading
