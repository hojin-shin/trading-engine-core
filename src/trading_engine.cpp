#include "trading/trading_engine.hpp"

#include <exception>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace trading {
namespace {

template <typename Writer>
void write_trace(std::ostream*& destination, std::uint64_t sequence,
                 Timestamp timestamp, Writer writer) noexcept {
    if (!destination) { return; }
    try {
        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << std::fixed << std::setprecision(2)
             << "[trace] seq=" << sequence << " timestamp_ns=" << timestamp << ' ';
        writer(line);
        *destination << line.str() << '\n';
        if (!*destination) { destination = nullptr; }
    } catch (...) {
        // Diagnostic output must not interrupt execution or shutdown cancellation.
        destination = nullptr;
    }
}

} // namespace

TradingEngine::TradingEngine(std::unique_ptr<IStrategy> strategy,
                             std::unique_ptr<IExecutionGateway> gateway,
                             RiskLimits limits, std::size_t queue_capacity, std::ostream* trace)
    : strategy_(std::move(strategy)), gateway_(std::move(gateway)), risk_(limits),
      queue_(queue_capacity), trace_(trace) {
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
        cancel_working_orders(consumer_error || producer_error ? "run_error" : "end_of_replay");
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
    write_trace(trace_, last_sequence_, last_timestamp_, [&](auto& out) {
        out << "event=TICK symbol=" << data.symbol << " bid=" << data.bid << " ask=" << data.ask
            << " bid_size=" << data.bid_size << " ask_size=" << data.ask_size;
    });
    positions_.mark(data);
    trace_position(data.symbol, "mark");
    apply_fills(gateway_->on_market_data(data));
    ++processed_ticks_;
    if (auto action = strategy_->on_market_data(data)) {
        if (const auto* cancel = std::get_if<CancelRequest>(&*action)) {
            process_cancel(*cancel);
            return;
        }
        if (const auto* replace = std::get_if<ReplaceRequest>(&*action)) {
            process_replace(*replace, data);
            return;
        }
        submit_signal(std::get<Signal>(*action), data);
    }
}

OrderId TradingEngine::submit_signal(const Signal& signal, const MarketData& data) {
    const auto id = orders_.create(signal);
    trace_order(orders_.get(id));
    const auto decision = risk_.check(signal, data, positions_, orders_);
    if (!decision.approved) {
        orders_.reject(id, decision.reason);
        trace_order(orders_.get(id));
    } else {
        orders_.accept(id);
        trace_order(orders_.get(id));
        apply_fills(gateway_->submit(orders_.get(id)));
    }
    strategy_->on_order_created(orders_.get(id));
    return id;
}

void TradingEngine::process_replace(const ReplaceRequest& request, const MarketData& data) {
    write_trace(trace_, last_sequence_, last_timestamp_, [&](auto& out) {
        out << "event=REPLACE_REQUEST order=" << request.order_id << " limit_price=" << request.limit_price;
    });
    ReplaceResult result{request.order_id, false, std::nullopt, false, {}};
    const auto found = orders_.orders().find(request.order_id);
    if (found == orders_.orders().end()) {
        result.reason = "Unknown order";
    } else if (!is_working(found->second.state)) {
        result.reason = "Order is not working";
    } else if (found->second.request.type != OrderType::Limit) {
        result.reason = "Replacement requires a limit order";
    } else if (!valid_price(request.limit_price)) {
        result.reason = "Invalid replacement price";
    } else if (found->second.request.symbol != data.symbol) {
        result.reason = "No matching current quote";
    } else {
        // Matching has already processed this tick. Preserve filled history and
        // prepare only the current remainder before asking the gateway to cancel.
        auto replacement = found->second.request;
        replacement.quantity = found->second.remaining();
        replacement.limit_price = request.limit_price;
        if (!gateway_->cancel(request.order_id)) {
            result.reason = "Gateway declined cancellation";
        } else {
            orders_.cancel(request.order_id);
            result.original_cancelled = true;
            trace_order(found->second, "replace_request");
            // The old reservation is now released. New admission can still fail;
            // never revive the original order if it does.
            const auto id = submit_signal(replacement, data);
            result.replacement_order_id = id;
            const auto& order = orders_.get(id);
            result.replaced = order.state != OrderState::Rejected;
            result.reason = result.replaced ? "replacement_submitted" : order.rejection_reason;
        }
    }
    replace_results_.push_back(std::move(result));
    write_trace(trace_, last_sequence_, last_timestamp_, [&](auto& out) {
        const auto& saved = replace_results_.back();
        out << "event=REPLACE_RESULT order=" << saved.order_id
            << " original_cancelled=" << (saved.original_cancelled ? "true" : "false");
        if (saved.replacement_order_id) { out << " replacement_order=" << *saved.replacement_order_id; }
        out << " result=" << (saved.replaced ? "Replaced" : "Rejected")
            << " reason=" << std::quoted(saved.reason);
    });
}

void TradingEngine::process_cancel(const CancelRequest& request) {
    write_trace(trace_, last_sequence_, last_timestamp_, [&](auto& out) {
        out << "event=CANCEL_REQUEST order=" << request.order_id;
    });
    CancelResult result{request.order_id, false, {}};
    const auto found = orders_.orders().find(request.order_id);
    if (found == orders_.orders().end()) {
        result.reason = "Unknown order";
    } else if (!is_working(found->second.state)) {
        result.reason = "Order is not working";
    } else if (!gateway_->cancel(request.order_id)) {
        result.reason = "Gateway declined cancellation";
    } else {
        orders_.cancel(request.order_id);
        result.cancelled = true;
        result.reason = "strategy_request";
        trace_order(found->second, result.reason);
    }
    cancel_results_.push_back(std::move(result));
    write_trace(trace_, last_sequence_, last_timestamp_, [&](auto& out) {
        const auto& saved = cancel_results_.back();
        out << "event=CANCEL_RESULT order=" << saved.order_id
            << " result=" << (saved.cancelled ? "Cancelled" : "Rejected")
            << " reason=" << std::quoted(saved.reason);
    });
}

void TradingEngine::apply_fills(const std::vector<Fill>& fills) {
    for (const auto& fill : fills) {
        orders_.apply_fill(fill);
        positions_.apply_fill(fill);
        fills_.push_back(fill);
        write_trace(trace_, last_sequence_, last_timestamp_, [&](auto& out) {
            out << "event=FILL order=" << fill.order_id << " symbol=" << fill.symbol
                << " side=" << (fill.side == Side::Buy ? "Buy" : "Sell")
                << " quantity=" << fill.quantity << " price=" << fill.price
                << " fill_timestamp_ns=" << fill.timestamp_ns;
        });
        trace_order(orders_.get(fill.order_id));
        trace_position(fill.symbol, "fill");
    }
}

void TradingEngine::cancel_working_orders(std::string_view reason) {
    for (const auto& [id, order] : orders_.orders()) {
        if (!is_working(order.state)) { continue; }
        if (!gateway_->cancel(id)) { throw std::runtime_error("Gateway could not cancel a working order"); }
        orders_.cancel(id);
        trace_order(order, reason);
    }
}

void TradingEngine::trace_order(const Order& order, std::string_view reason) noexcept {
    write_trace(trace_, last_sequence_, last_timestamp_, [&](auto& out) {
        out << "event=ORDER order=" << order.id << " symbol=" << order.request.symbol
            << " side=" << (order.request.side == Side::Buy ? "Buy" : "Sell")
            << " type=" << (order.request.type == OrderType::Market ? "Market" : "Limit")
            << " state=" << to_string(order.state) << " quantity=" << order.request.quantity
            << " filled=" << order.filled_quantity << " unfilled=" << order.remaining()
            << " average_price=" << order.average_fill_price;
        if (order.request.limit_price) { out << " limit_price=" << *order.request.limit_price; }
        if (!order.rejection_reason.empty()) {
            out << " reason=" << std::quoted(order.rejection_reason);
        } else if (!reason.empty()) {
            out << " reason=" << reason;
        }
    });
}

void TradingEngine::trace_position(const std::string& symbol, std::string_view cause) noexcept {
    write_trace(trace_, last_sequence_, last_timestamp_, [&](auto& out) {
        const auto position = positions_.get(symbol);
        out << "event=POSITION cause=" << cause << " symbol=" << symbol
            << " quantity=" << position.quantity << " average_price=" << position.average_price
            << " mark_price=" << position.mark_price << " realized_pnl=" << position.realized_pnl
            << " unrealized_pnl=" << position.unrealized_pnl;
    });
}

} // namespace trading
