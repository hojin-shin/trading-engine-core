#pragma once

#include "trading/types.hpp"

#include <vector>

namespace trading {

// Synchronous simulation contract: submit accepts or throws; reports are incremental fills.
class IExecutionGateway {
public:
    virtual ~IExecutionGateway() = default;
    virtual std::vector<Fill> on_market_data(const MarketData& data) = 0;
    virtual std::vector<Fill> submit(const Order& order) = 0;
    virtual bool cancel(OrderId id) = 0;
};

} // namespace trading
