#pragma once

#include "trading/types.hpp"
#include <map>
#include <string>
#include <vector>

namespace trading {

struct Instrument {
    std::string symbol;
    double tick_size{};
    double contract_multiplier{};
};

// Immutable after construction. Prices are positive multiples of a constant tick.
// No implicit fallback for unknown symbols. All instruments use one accounting unit.
class InstrumentCatalog {
public:
    InstrumentCatalog(); // Synthetic fixtures only: SYNTH and OTHER, tick .01, multiplier 1.
    explicit InstrumentCatalog(std::vector<Instrument> instruments);
    [[nodiscard]] const Instrument& get(const std::string& symbol) const;
    void validate_price(const std::string& symbol, double price) const;
    void validate_quote(const MarketData& data) const;
    void validate_order(const Signal& signal) const;
    void validate_execution(const Fill& fill) const;

private:
    std::map<std::string, Instrument> instruments_;
};

} // namespace trading
