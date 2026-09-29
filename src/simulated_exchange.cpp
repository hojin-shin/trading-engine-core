#include "trading/simulated_exchange.hpp"

#include <algorithm>
#include <stdexcept>

namespace trading {

std::vector<Fill> SimulatedExchange::on_market_data(const MarketData& data) {
    validate_market_data(data);
    if (data.sequence <= last_sequence_ || data.timestamp_ns < last_timestamp_) {
        throw std::invalid_argument("Exchange received out-of-order market data");
    }
    last_sequence_ = data.sequence;
    last_timestamp_ = data.timestamp_ns;
    quotes_[data.symbol] = Quote{data, data.bid_size, data.ask_size};
    return match(data.symbol);
}

std::vector<Fill> SimulatedExchange::submit(const Order& order) {
    validate_signal(order.request);
    if (order.id == 0 || order.state != OrderState::Accepted || order.filled_quantity != 0 ||
        seen_ids_.contains(order.id) || !quotes_.contains(order.request.symbol)) {
        throw std::invalid_argument("Exchange requires a new accepted order and a current quote");
    }
    seen_ids_.insert(order.id);
    working_.emplace(order.id, order);
    return match(order.request.symbol);
}

bool SimulatedExchange::cancel(OrderId id) { return working_.erase(id) != 0; }

std::vector<Fill> SimulatedExchange::match(const std::string& symbol) {
    auto& quote = quotes_.at(symbol);
    std::vector<Fill> fills;
    for (auto it = working_.begin(); it != working_.end();) {
        auto& order = it->second;
        if (order.request.symbol != symbol) { ++it; continue; }
        const bool buy = order.request.side == Side::Buy;
        const double price = buy ? quote.data.ask : quote.data.bid;
        auto& available = buy ? quote.ask_remaining : quote.bid_remaining;
        const bool executable = order.request.type == OrderType::Market ||
            (buy ? price <= *order.request.limit_price : price >= *order.request.limit_price);
        if (executable && available > 0) {
            const auto quantity = std::min(order.remaining(), available);
            fills.push_back({order.id, symbol, order.request.side, quantity, price, quote.data.timestamp_ns});
            available -= quantity;
            order.filled_quantity += quantity;
        }
        if (order.remaining() == 0) { it = working_.erase(it); } else { ++it; }
    }
    return fills;
}

} // namespace trading
