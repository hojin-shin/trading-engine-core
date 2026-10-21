#pragma once

#include "trading/types.hpp"

#include <istream>
#include <optional>
#include <vector>

namespace trading {

class IMarketDataSource {
public:
    virtual ~IMarketDataSource() = default;
    virtual std::optional<MarketData> next() = 0;
};

class ReplayMarketDataSource final : public IMarketDataSource {
public:
    explicit ReplayMarketDataSource(std::vector<MarketData> data);
    static ReplayMarketDataSource from_csv(std::istream& input);
    std::optional<MarketData> next() override;

private:
    std::vector<MarketData> data_;
    std::size_t cursor_{};
};

// Reads the header at construction and one validated row per next().
// The borrowed stream must outlive this source and run(); only the producer may read it.
// EOF is stable. Any read/parse failure makes the source terminal (no skipping bad rows).
class CsvReplayMarketDataSource final : public IMarketDataSource {
public:
    explicit CsvReplayMarketDataSource(std::istream& input);
    std::optional<MarketData> next() override;

private:
    std::istream& input_;
    std::size_t line_number_{1};
    std::uint64_t last_sequence_{};
    Timestamp last_timestamp_{};
    bool eof_{};
    bool failed_{};
};

[[nodiscard]] std::vector<MarketData> sample_market_data();

} // namespace trading
