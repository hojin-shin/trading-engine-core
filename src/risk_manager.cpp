#include "trading/risk_manager.hpp"

#include <cmath>
#include <stdexcept>

namespace trading {

RiskManager::RiskManager(RiskLimits limits) : limits_(limits) {
    if (limits.max_order_quantity <= 0 || limits.max_order_quantity > max_quantity ||
        limits.max_absolute_position <= 0 || limits.max_absolute_position > max_quantity ||
        !std::isfinite(limits.max_order_notional) || limits.max_order_notional <= 0.0 ||
        !std::isfinite(limits.max_loss) || limits.max_loss <= 0.0) {
        throw std::invalid_argument("Invalid risk limits");
    }
}

RiskDecision RiskManager::check(const Signal& signal, const MarketData& quote,
                               const PositionManager& positions, const OrderManager& orders) const {
    try {
        validate_signal(signal);
        validate_market_data(quote);
    } catch (const std::invalid_argument& error) {
        return {false, error.what()};
    }
    if (signal.symbol != quote.symbol) { return {false, "No matching current quote"}; }
    if (signal.quantity > limits_.max_order_quantity) { return {false, "Order quantity limit"}; }
    const auto price = signal.type == OrderType::Limit ? *signal.limit_price :
                       (signal.side == Side::Buy ? quote.ask : quote.bid);
    if (price * static_cast<double>(signal.quantity) > limits_.max_order_notional) {
        return {false, "Order notional limit"};
    }
    const auto pnl = positions.total_pnl();
    if (!std::isfinite(pnl) || pnl <= -limits_.max_loss) { return {false, "Session loss limit"}; }

    // Opposing working orders cannot offset one another: either side may fill alone.
    const auto position = positions.get(signal.symbol).quantity;
    Quantity buy_capacity = limits_.max_absolute_position - position;
    Quantity sell_capacity = limits_.max_absolute_position + position;
    for (const auto& [id, order] : orders.orders()) {
        (void)id;
        if (order.request.symbol != signal.symbol || !is_working(order.state)) { continue; }
        auto& capacity = order.request.side == Side::Buy ? buy_capacity : sell_capacity;
        if (order.remaining() > capacity) { return {false, "Working order exposure limit"}; }
        capacity -= order.remaining();
    }
    const auto capacity = signal.side == Side::Buy ? buy_capacity : sell_capacity;
    if (signal.quantity > capacity) { return {false, "Position limit including working orders"}; }
    return {true, {}};
}

} // namespace trading
