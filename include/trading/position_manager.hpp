#pragma once

#include "trading/instrument_catalog.hpp"

#include <map>
#include <string>

namespace trading {

class PositionManager {
public:
    PositionManager() = default;
    explicit PositionManager(InstrumentCatalog instruments);
    [[nodiscard]] const InstrumentCatalog& instruments() const { return instruments_; }
    void mark(const MarketData& data);
    void apply_fill(const Fill& fill);
    [[nodiscard]] Position get(const std::string& symbol) const;
    [[nodiscard]] double total_pnl() const;
    [[nodiscard]] const std::map<std::string, Position>& positions() const { return positions_; }

private:
    InstrumentCatalog instruments_;
    std::map<std::string, Position> positions_;
};

} // namespace trading
