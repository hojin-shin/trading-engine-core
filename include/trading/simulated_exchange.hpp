#pragma once

#include "trading/execution_gateway.hpp"

#include <map>
#include <set>
#include <string>

namespace trading {

class SimulatedExchange final : public IExecutionGateway {
public:
    std::vector<Fill> on_market_data(const MarketData& data) override;
    std::vector<Fill> submit(const Order& order) override;
    bool cancel(OrderId id) override;

private:
    struct Quote {
        MarketData data;
        Quantity bid_remaining{};
        Quantity ask_remaining{};
    };
    std::vector<Fill> match(const std::string& symbol);
    std::map<std::string, Quote> quotes_;
    std::map<OrderId, Order> working_;
    std::set<OrderId> seen_ids_;
    std::uint64_t last_sequence_{};
    Timestamp last_timestamp_{};
};

} // namespace trading
