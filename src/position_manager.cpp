#include "trading/position_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace trading {
namespace {

void revalue(Position& position, double multiplier) {
    position.unrealized_pnl = static_cast<double>(position.quantity) *
                              (position.mark_price - position.average_price) * multiplier;
}

} // namespace

PositionManager::PositionManager(InstrumentCatalog instruments) : instruments_(std::move(instruments)) {}

void PositionManager::mark(const MarketData& data) {
    instruments_.validate_quote(data);
    auto& position = positions_[data.symbol];
    position.symbol = data.symbol;
    position.mark_price = data.mid();
    revalue(position, instruments_.get(data.symbol).contract_multiplier);
}

void PositionManager::apply_fill(const Fill& fill) {
    instruments_.validate_execution(fill);
    const auto multiplier = instruments_.get(fill.symbol).contract_multiplier;
    auto position = get(fill.symbol);
    const auto delta = fill.side == Side::Buy ? fill.quantity : -fill.quantity;
    const auto old_quantity = position.quantity;
    const auto new_quantity = old_quantity + delta;
    if (new_quantity > max_quantity || new_quantity < -max_quantity) {
        throw std::overflow_error("Position exceeds supported quantity range");
    }
    if (old_quantity == 0 || (old_quantity > 0) == (delta > 0)) {
        position.average_price =
            (static_cast<double>(std::abs(old_quantity)) * position.average_price +
             static_cast<double>(fill.quantity) * fill.price) /
            static_cast<double>(std::abs(new_quantity));
    } else {
        const auto closed = std::min(std::abs(old_quantity), fill.quantity);
        position.realized_pnl += static_cast<double>(closed) *
            (fill.price - position.average_price) * (old_quantity > 0 ? 1.0 : -1.0) * multiplier;
        if (new_quantity == 0) {
            position.average_price = 0.0;
        } else if ((new_quantity > 0) != (old_quantity > 0)) {
            position.average_price = fill.price;
        }
    }
    position.quantity = new_quantity;
    if (position.mark_price == 0.0) { position.mark_price = fill.price; }
    revalue(position, multiplier);
    positions_[fill.symbol] = position;
}

Position PositionManager::get(const std::string& symbol) const {
    const auto found = positions_.find(symbol);
    return found == positions_.end() ? Position{symbol, 0, 0.0, 0.0, 0.0, 0.0} : found->second;
}

double PositionManager::total_pnl() const {
    double total = 0.0;
    for (const auto& [symbol, position] : positions_) {
        (void)symbol;
        total += position.realized_pnl + position.unrealized_pnl;
    }
    return total;
}

} // namespace trading
