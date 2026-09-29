#include "trading/trading_engine.hpp"

#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

namespace trading {

TradingEngine::TradingEngine(std::unique_ptr<IStrategy> strategy,
                             std::unique_ptr<IExecutionGateway> gateway,
                             RiskLimits limits, std::size_t queue_capacity)
    : strategy_(std::move(strategy)), gateway_(std::move(gateway)), risk_(limits), queue_(queue_capacity) {
    if (!strategy_ || !gateway_) { throw std::invalid_argument("Engine dependencies must not be null"); }
}

void TradingEngine::run(IMarketDataSource& source) {
    if (started_) { throw std::logic_error("TradingEngine is single-use"); }
    started_ = true;
    std::exception_ptr producer_error;
    std::exception_ptr consumer_error;
    std::jthread producer([&] {
        try {
            while (auto tick = source.next()) {
                if (!queue_.push(std::move(*tick))) { break; }
            }
        } catch (...) {
            producer_error = std::current_exception();
        }
        queue_.close();
    });
    try {
        while (auto tick = queue_.pop()) { process(*tick); }
    } catch (...) {
        consumer_error = std::current_exception();
        queue_.close();
    }
    producer.join();
    try {
        cancel_working_orders();
    } catch (...) {
        if (!consumer_error) { consumer_error = std::current_exception(); }
    }
    if (consumer_error) { std::rethrow_exception(consumer_error); }
    if (producer_error) { std::rethrow_exception(producer_error); }
}

void TradingEngine::process(const MarketData& data) {
    validate_market_data(data);
    if (data.sequence <= last_sequence_ || data.timestamp_ns < last_timestamp_) {
        throw std::invalid_argument("Engine received out-of-order market data");
    }
    last_sequence_ = data.sequence;
    last_timestamp_ = data.timestamp_ns;
    positions_.mark(data);
    apply_fills(gateway_->on_market_data(data));
    ++processed_ticks_;
    if (auto signal = strategy_->on_market_data(data)) {
        const auto id = orders_.create(*signal);
        const auto decision = risk_.check(*signal, data, positions_, orders_);
        if (!decision.approved) {
            orders_.reject(id, decision.reason);
            return;
        }
        orders_.accept(id);
        apply_fills(gateway_->submit(orders_.get(id)));
    }
}

void TradingEngine::apply_fills(const std::vector<Fill>& fills) {
    for (const auto& fill : fills) {
        orders_.apply_fill(fill);
        positions_.apply_fill(fill);
        fills_.push_back(fill);
    }
}

void TradingEngine::cancel_working_orders() {
    for (const auto& [id, order] : orders_.orders()) {
        if (!is_working(order.state)) { continue; }
        if (!gateway_->cancel(id)) { throw std::runtime_error("Gateway could not cancel a working order"); }
        orders_.cancel(id);
    }
}

} // namespace trading
