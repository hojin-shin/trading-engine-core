#include "trading/order_manager.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace trading {

OrderManager::OrderManager(InstrumentCatalog instruments) : instruments_(std::move(instruments)) {}

OrderId OrderManager::create(Signal request) {
    if (next_id_ == std::numeric_limits<OrderId>::max()) {
        throw std::overflow_error("Order identifiers exhausted");
    }
    const auto id = next_id_++;
    orders_.emplace(id, Order{id, std::move(request), OrderState::PendingRisk, 0, 0.0, {}});
    return id;
}

void OrderManager::accept(OrderId id) {
    auto& order = orders_.at(id);
    if (order.state != OrderState::PendingRisk) { throw std::logic_error("Order is not pending risk"); }
    instruments_.validate_order(order.request);
    order.state = OrderState::Accepted;
}

void OrderManager::reject(OrderId id, std::string reason) {
    auto& order = orders_.at(id);
    if (order.state != OrderState::PendingRisk || reason.empty()) {
        throw std::logic_error("Rejection requires a pending order and a reason");
    }
    order.rejection_reason = std::move(reason);
    order.state = OrderState::Rejected;
}

void OrderManager::cancel(OrderId id) {
    auto& order = orders_.at(id);
    if (!is_working(order.state)) { throw std::logic_error("Only working orders can be cancelled"); }
    order.state = OrderState::Cancelled;
}

bool OrderManager::apply_fill(const Fill& fill) {
    validate_fill(fill);
    if (const auto seen = executions_.find(fill.execution_id); seen != executions_.end()) {
        if (seen->second != fill) {
            throw std::logic_error("Conflicting report for execution ID");
        }
        return false;
    }
    instruments_.validate_execution(fill);
    auto& order = orders_.at(fill.order_id);
    if (!is_working(order.state) || fill.symbol != order.request.symbol ||
        fill.side != order.request.side || fill.quantity > order.remaining() ||
        (order.request.type == OrderType::Limit &&
         ((fill.side == Side::Buy && fill.price > *order.request.limit_price) ||
          (fill.side == Side::Sell && fill.price < *order.request.limit_price)))) {
        throw std::logic_error("Fill does not match a working order");
    }
    // Allocate the deduplication record before changing the order.
    executions_.emplace(fill.execution_id, fill);
    const auto total = order.filled_quantity + fill.quantity;
    order.average_fill_price =
        (order.average_fill_price * static_cast<double>(order.filled_quantity) +
         fill.price * static_cast<double>(fill.quantity)) / static_cast<double>(total);
    order.filled_quantity = total;
    order.state = order.remaining() == 0 ? OrderState::Filled : OrderState::PartiallyFilled;
    return true;
}

const Order& OrderManager::get(OrderId id) const { return orders_.at(id); }

} // namespace trading
