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

[[nodiscard]] std::vector<MarketData> sample_market_data();

} // namespace trading
