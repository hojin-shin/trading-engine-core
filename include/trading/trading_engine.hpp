#pragma once

#include "trading/execution_gateway.hpp"
#include "trading/replay_market_data_source.hpp"
#include "trading/risk_manager.hpp"
#include "trading/strategy.hpp"
#include "trading/thread_safe_queue.hpp"

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string_view>
#include <vector>

namespace trading {

class TradingEngine {
public:
    TradingEngine(std::unique_ptr<IStrategy> strategy, std::unique_ptr<IExecutionGateway> gateway,
                  RiskLimits limits = {}, std::size_t queue_capacity = 256,
                  std::ostream* trace = nullptr);
    // Optional trace is synchronous and best-effort. The stream must outlive run().
    // Only the consumer writes to it; do not access the stream concurrently.
    // One run per engine. Accessors are for use after run() returns or throws.
    void run(IMarketDataSource& source);
    [[nodiscard]] const OrderManager& orders() const { return orders_; }
    [[nodiscard]] const PositionManager& positions() const { return positions_; }
    [[nodiscard]] const std::vector<Fill>& fills() const { return fills_; }
    [[nodiscard]] const std::vector<CancelResult>& cancel_results() const { return cancel_results_; }
    [[nodiscard]] std::size_t processed_ticks() const { return processed_ticks_; }

private:
    void process(const MarketData& data);
    void process_cancel(const CancelRequest& request);
    void apply_fills(const std::vector<Fill>& fills);
    void cancel_working_orders(std::string_view reason);
    void trace_order(const Order& order, std::string_view reason = {}) noexcept;
    void trace_position(const std::string& symbol, std::string_view cause) noexcept;

    std::unique_ptr<IStrategy> strategy_;
    std::unique_ptr<IExecutionGateway> gateway_;
    RiskManager risk_;
    OrderManager orders_;
    PositionManager positions_;
    ThreadSafeQueue<MarketData> queue_;
    std::vector<Fill> fills_;
    std::vector<CancelResult> cancel_results_;
    std::size_t processed_ticks_{};
    std::uint64_t last_sequence_{};
    Timestamp last_timestamp_{};
    bool started_{};
    std::ostream* trace_{};
};

} // namespace trading
