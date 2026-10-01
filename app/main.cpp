#include "trading/simulated_exchange.hpp"
#include "trading/trading_engine.hpp"

#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

int main(int argc, char* argv[]) {
    try {
        constexpr auto usage = "Usage: trading_demo [--trace] [--limit-demo | --cancel-demo] [replay.csv]";
        bool trace = false;
        bool limit_demo = false;
        bool cancel_demo = false;
        const char* replay_path = nullptr;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if (argument == "--trace" && !trace) {
                trace = true;
            } else if (argument == "--limit-demo" && !limit_demo) {
                limit_demo = true;
            } else if (argument == "--cancel-demo" && !cancel_demo) {
                cancel_demo = true;
            } else if (argument == "--help") {
                std::cout << usage << '\n'
                          << "--limit-demo requires a CSV: buy 2 at limit 100 on tick 1, "
                             "sell 2 at limit 104 on tick 3.\n"
                          << "--cancel-demo requires a CSV: buy 2 at limit 100 on tick 1, "
                             "request cancellation on tick 2.\n";
                return 0;
            } else if (!argument.empty() && !argument.starts_with('-') && !replay_path) {
                replay_path = argv[i];
            } else {
                throw std::invalid_argument(usage);
            }
        }
        if (limit_demo && cancel_demo) {
            throw std::invalid_argument("Choose either --limit-demo or --cancel-demo");
        }
        if (cancel_demo && !replay_path) {
            throw std::invalid_argument("--cancel-demo requires a replay CSV, e.g. data/sample_cancel.csv");
        }
        if (limit_demo && !replay_path) {
            throw std::invalid_argument("--limit-demo requires a replay CSV, e.g. data/sample_limit.csv");
        }
        auto source = trading::ReplayMarketDataSource(trading::sample_market_data());
        if (replay_path) {
            std::ifstream input(replay_path);
            if (!input) { throw std::runtime_error("Cannot open replay file"); }
            source = trading::ReplayMarketDataSource::from_csv(input);
        }
        const auto limits = limit_demo
            ? std::optional<trading::ExampleLimitPrices>{{100.0, 104.0}} : std::nullopt;
        std::unique_ptr<trading::IStrategy> strategy;
        if (cancel_demo) {
            strategy = std::make_unique<trading::CancelExampleStrategy>();
        } else {
            strategy = std::make_unique<trading::ExampleStrategy>("SYNTH", 2, limits);
        }
        trading::TradingEngine engine(std::move(strategy),
                                     std::make_unique<trading::SimulatedExchange>(),
                                     trading::RiskLimits{}, 256, trace ? &std::cout : nullptr);
        engine.run(source);
        std::cout << "ticks=" << engine.processed_ticks() << " orders=" << engine.orders().orders().size()
                  << " fills=" << engine.fills().size() << '\n' << std::fixed << std::setprecision(2);
        for (const auto& [id, order] : engine.orders().orders()) {
            std::cout << "order=" << id << " state=" << trading::to_string(order.state)
                      << " filled=" << order.filled_quantity << " average_price=" << order.average_fill_price;
            if (!order.rejection_reason.empty()) { std::cout << " reason=" << order.rejection_reason; }
            std::cout << '\n';
        }
        for (const auto& result : engine.cancel_results()) {
            std::cout << "cancel=" << result.order_id
                      << " result=" << (result.cancelled ? "Cancelled" : "Rejected")
                      << " reason=" << std::quoted(result.reason) << '\n';
        }
        for (const auto& [symbol, position] : engine.positions().positions()) {
            std::cout << symbol << " position=" << position.quantity
                      << " realized_pnl=" << position.realized_pnl
                      << " unrealized_pnl=" << position.unrealized_pnl << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
