#include "trading/simulated_exchange.hpp"
#include "trading/trading_engine.hpp"

#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>

int main(int argc, char* argv[]) {
    try {
        if (argc > 2) { throw std::invalid_argument("Usage: trading_demo [replay.csv]"); }
        auto source = trading::ReplayMarketDataSource(trading::sample_market_data());
        if (argc == 2) {
            std::ifstream input(argv[1]);
            if (!input) { throw std::runtime_error("Cannot open replay file"); }
            source = trading::ReplayMarketDataSource::from_csv(input);
        }
        trading::TradingEngine engine(std::make_unique<trading::ExampleStrategy>(),
                                     std::make_unique<trading::SimulatedExchange>());
        engine.run(source);
        std::cout << "ticks=" << engine.processed_ticks() << " orders=" << engine.orders().orders().size()
                  << " fills=" << engine.fills().size() << '\n' << std::fixed << std::setprecision(2);
        for (const auto& [id, order] : engine.orders().orders()) {
            std::cout << "order=" << id << " state=" << trading::to_string(order.state)
                      << " filled=" << order.filled_quantity << " average_price=" << order.average_fill_price;
            if (!order.rejection_reason.empty()) { std::cout << " reason=" << order.rejection_reason; }
            std::cout << '\n';
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
