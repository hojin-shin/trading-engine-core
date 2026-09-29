#pragma once

#include "trading/types.hpp"

#include <map>
#include <string>

namespace trading {

class PositionManager {
public:
    void mark(const MarketData& data);
    void apply_fill(const Fill& fill);
    [[nodiscard]] Position get(const std::string& symbol) const;
    [[nodiscard]] double total_pnl() const;
    [[nodiscard]] const std::map<std::string, Position>& positions() const { return positions_; }

private:
    std::map<std::string, Position> positions_;
};

} // namespace trading
