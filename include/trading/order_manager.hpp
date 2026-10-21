#pragma once

#include "trading/types.hpp"

#include <map>
#include <string>

namespace trading {

class OrderManager {
public:
    OrderId create(Signal request);
    void accept(OrderId id);
    void reject(OrderId id, std::string reason);
    void cancel(OrderId id);
    // True for a newly applied fill; false for an identical retransmission.
    bool apply_fill(const Fill& fill);
    [[nodiscard]] const Order& get(OrderId id) const;
    [[nodiscard]] const std::map<OrderId, Order>& orders() const { return orders_; }

private:
    std::map<ExecutionId, Fill> executions_;
    OrderId next_id_{1};
    std::map<OrderId, Order> orders_;
};

} // namespace trading
