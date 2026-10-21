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
        constexpr auto usage = "Usage: trading_demo [--trace] [--contract-demo] [--limit-demo | --cancel-demo | --replace-demo] [replay.csv]";
        bool trace = false;
        bool contract_demo = false;
        bool limit_demo = false;
        bool cancel_demo = false;
        bool replace_demo = false;
        const char* replay_path = nullptr;
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if (argument == "--trace" && !trace) {
                trace = true;
            } else if (argument == "--contract-demo" && !contract_demo) {
                contract_demo = true;
            } else if (argument == "--limit-demo" && !limit_demo) {
                limit_demo = true;
            } else if (argument == "--cancel-demo" && !cancel_demo) {
                cancel_demo = true;
            } else if (argument == "--replace-demo" && !replace_demo) {
                replace_demo = true;
            } else if (argument == "--help") {
                std::cout << usage << '\n'
                          << "--contract-demo uses synthetic SYNTH tick 0.25 and multiplier 10.\n"
                          << "--limit-demo requires a CSV: buy 2 at limit 100 on tick 1, "
                             "sell 2 at limit 104 on tick 3.\n"
                          << "--cancel-demo requires a CSV: buy 2 at limit 100 on tick 1, "
                             "request cancellation on tick 2.\n"
                          << "--replace-demo requires a CSV: buy 2 at limit 100 on tick 1, "
                             "replace the remainder at limit 101 on tick 2.\n";
                return 0;
            } else if (!argument.empty() && !argument.starts_with('-') && !replay_path) {
                replay_path = argv[i];
            } else {
                throw std::invalid_argument(usage);
            }
        }
        if (static_cast<int>(limit_demo) + static_cast<int>(cancel_demo) + static_cast<int>(replace_demo) > 1) {
            throw std::invalid_argument("Choose one demo mode");
        }
        if (replace_demo && !replay_path) {
            throw std::invalid_argument("--replace-demo requires a replay CSV, e.g. data/sample_replace.csv");
        }
        if (cancel_demo && !replay_path) {
            throw std::invalid_argument("--cancel-demo requires a replay CSV, e.g. data/sample_cancel.csv");
        }
        if (limit_demo && !replay_path) {
            throw std::invalid_argument("--limit-demo requires a replay CSV, e.g. data/sample_limit.csv");
        }
        // Keep the file alive until run() has joined the replay producer.
        std::ifstream input;
        std::unique_ptr<trading::IMarketDataSource> source;
        if (replay_path) {
            input.open(replay_path);
            if (!input) { throw std::runtime_error("Cannot open replay file"); }
            source = std::make_unique<trading::CsvReplayMarketDataSource>(input);
        } else {
            source = std::make_unique<trading::ReplayMarketDataSource>(trading::sample_market_data());
        }
        const auto limits = limit_demo
            ? std::optional<trading::ExampleLimitPrices>{{100.0, 104.0}} : std::nullopt;
        std::unique_ptr<trading::IStrategy> strategy;
        if (replace_demo) {
            strategy = std::make_unique<trading::ReplaceExampleStrategy>();
        } else if (cancel_demo) {
            strategy = std::make_unique<trading::CancelExampleStrategy>();
        } else {
            strategy = std::make_unique<trading::ExampleStrategy>("SYNTH", 2, limits);
        }
        auto instruments = contract_demo
            ? trading::InstrumentCatalog(std::vector<trading::Instrument>{{"SYNTH", 0.25, 10}, {"OTHER", 0.01, 1}})
            : trading::InstrumentCatalog{};
        if (contract_demo) {
            std::cout << "instrument=SYNTH tick_size=0.25 contract_multiplier=10.00\n";
        }
        trading::TradingEngine engine(std::move(strategy),
                                     std::make_unique<trading::SimulatedExchange>(),
                                     trading::RiskLimits{}, 256, trace ? &std::cout : nullptr, std::move(instruments));
        engine.run(*source);
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
        for (const auto& result : engine.replace_results()) {
            std::cout << "replace=" << result.order_id
                      << " original_cancelled=" << (result.original_cancelled ? "true" : "false");
            if (result.replacement_order_id) { std::cout << " replacement_order=" << *result.replacement_order_id; }
            std::cout << " result=" << (result.replaced ? "Replaced" : "Rejected")
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
