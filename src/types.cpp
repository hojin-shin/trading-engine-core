#include "trading/types.hpp"

#include <cmath>
#include <stdexcept>

namespace trading {

bool valid_price(double value) {
    return std::isfinite(value) && value > 0.0 && value <= max_price;
}

bool valid_side(Side side) { return side == Side::Buy || side == Side::Sell; }

bool is_working(OrderState state) {
    return state == OrderState::Accepted || state == OrderState::PartiallyFilled;
}

std::string_view to_string(OrderState state) {
    switch (state) {
    case OrderState::PendingRisk: return "PendingRisk";
    case OrderState::Accepted: return "Accepted";
    case OrderState::PartiallyFilled: return "PartiallyFilled";
    case OrderState::Filled: return "Filled";
    case OrderState::Cancelled: return "Cancelled";
    case OrderState::Rejected: return "Rejected";
    }
    return "Unknown";
}

void validate_market_data(const MarketData& data) {
    if (data.sequence == 0 || data.symbol.empty() || !valid_price(data.bid) ||
        !valid_price(data.ask) || data.bid > data.ask || data.bid_size < 0 ||
        data.ask_size < 0 || data.bid_size > max_quantity || data.ask_size > max_quantity) {
        throw std::invalid_argument("Invalid market data");
    }
}

void validate_signal(const Signal& signal) {
    if (signal.symbol.empty() || !valid_side(signal.side) || signal.quantity <= 0 ||
        signal.quantity > max_quantity ||
        (signal.type != OrderType::Market && signal.type != OrderType::Limit) ||
        (signal.type == OrderType::Market && signal.limit_price.has_value()) ||
        (signal.type == OrderType::Limit &&
         (!signal.limit_price || !valid_price(*signal.limit_price)))) {
        throw std::invalid_argument("Invalid order request");
    }
}

void validate_fill(const Fill& fill) {
    if (fill.order_id == 0 || fill.symbol.empty() || !valid_side(fill.side) ||
        fill.quantity <= 0 || fill.quantity > max_quantity || !valid_price(fill.price)) {
        throw std::invalid_argument("Invalid fill");
    }
}

} // namespace trading
