#include "trading/instrument_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace trading {

InstrumentCatalog::InstrumentCatalog()
    : InstrumentCatalog(std::vector<Instrument>{{"SYNTH", 0.01, 1}, {"OTHER", 0.01, 1}}) {}

InstrumentCatalog::InstrumentCatalog(std::vector<Instrument> instruments) {
    for (auto& instrument : instruments) {
        if (instrument.symbol.empty() || !valid_price(instrument.tick_size) ||
            !valid_price(instrument.contract_multiplier)) {
            throw std::invalid_argument("Invalid instrument metadata");
        }
        const auto symbol = instrument.symbol;
        if (!instruments_.emplace(symbol, std::move(instrument)).second) {
            throw std::invalid_argument("Duplicate instrument symbol: " + symbol);
        }
    }
}

const Instrument& InstrumentCatalog::get(const std::string& symbol) const {
    const auto found = instruments_.find(symbol);
    if (found == instruments_.end()) {
        throw std::invalid_argument("Unknown instrument: " + symbol);
    }
    return found->second;
}

void InstrumentCatalog::validate_price(const std::string& symbol, double price) const {
    const auto& instrument = get(symbol);
    if (!valid_price(price)) { throw std::invalid_argument("Invalid instrument price"); }
    const auto ticks = price / instrument.tick_size;
    // Bound representable tick counts so floating-point tolerance stays far below a tick.
    if (!std::isfinite(ticks) || ticks > 1e12) {
        throw std::invalid_argument("Price exceeds supported tick count: " + symbol);
    }
    const auto nearest = std::round(ticks);
    const auto tolerance = 8 * std::numeric_limits<double>::epsilon() * std::max(1.0, ticks);
    if (nearest < 1 || std::abs(ticks - nearest) > tolerance) {
        throw std::invalid_argument("Price is off tick grid: " + symbol);
    }
}

void InstrumentCatalog::validate_quote(const MarketData& data) const {
    validate_market_data(data);
    validate_price(data.symbol, data.bid);
    validate_price(data.symbol, data.ask);
}

void InstrumentCatalog::validate_order(const Signal& signal) const {
    validate_signal(signal);
    (void)get(signal.symbol);
    if (signal.type == OrderType::Limit) { validate_price(signal.symbol, *signal.limit_price); }
}

void InstrumentCatalog::validate_execution(const Fill& fill) const {
    validate_fill(fill);
    validate_price(fill.symbol, fill.price);
}

} // namespace trading
