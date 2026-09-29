#include "trading/simulated_exchange.hpp"
#include "trading/trading_engine.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <stdexcept>

using namespace trading;

namespace {
constexpr auto header = "sequence,timestamp_ns,symbol,bid,ask,bid_size,ask_size\n";

class LimitStrategy final : public IStrategy {
public:
    std::optional<Signal> on_market_data(const MarketData& data) override {
        if (data.sequence > 2) { return std::nullopt; }
        return Signal{data.symbol, Side::Buy, 4, OrderType::Limit, 100};
    }
};

class ThrowingStrategy final : public IStrategy {
public:
    std::optional<Signal> on_market_data(const MarketData&) override {
        throw std::runtime_error("Strategy failed");
    }
};

class ThrowingSource final : public IMarketDataSource {
public:
    std::optional<MarketData> next() override {
        if (first_) { first_ = false; return sample_market_data().front(); }
        throw std::runtime_error("Source failed");
    }
private:
    bool first_{true};
};

class UncheckedSource final : public IMarketDataSource {
public:
    std::optional<MarketData> next() override { return sample_market_data().front(); }
};

} // namespace

TEST(Replay, ReadsStrictCsvAndReachesEof) {
    std::istringstream input(std::string(header) + "1,1000,SYNTH,99,101,10,20\r\n2,1000,OTHER,1,2,0,0\n");
    auto replay = ReplayMarketDataSource::from_csv(input);
    const auto first = replay.next();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->symbol, "SYNTH");
    EXPECT_EQ(first->ask_size, 20);
    EXPECT_EQ(replay.next()->symbol, "OTHER");
    EXPECT_FALSE(replay.next());
    EXPECT_FALSE(replay.next());
}

TEST(Replay, RejectsMalformedOrOutOfOrderRecords) {
    for (const auto* row : {"1,1000,SYNTH,99x,101,10,10", "1,1000,SYNTH,nan,101,10,10",
                            "1,1000,SYNTH,102,101,10,10", "1,1000,SYNTH,99,101,-1,10",
                            "1,1000,,99,101,10,10", "1,1000,SYNTH,99,101,10",
                            "1,1000,SYNTH,99,101,10,10,extra", "-1,1000,SYNTH,99,101,10,10",
                            "1,1000,SYNTH,99,101,10,10\n1,2000,SYNTH,99,101,10,10",
                            "1,1000,SYNTH,99,101,10,10\n2,999,SYNTH,99,101,10,10"}) {
        SCOPED_TRACE(row);
        std::istringstream input(std::string(header) + row + "\n");
        EXPECT_THROW(ReplayMarketDataSource::from_csv(input), std::invalid_argument);
    }
    std::istringstream wrong_header("wrong\n");
    EXPECT_THROW(ReplayMarketDataSource::from_csv(wrong_header), std::invalid_argument);
}

TEST(Replay, ReportsFailingLineNumber) {
    std::istringstream input(std::string(header) + "1,1000,SYNTH,99,bad,10,10\n");
    try {
        (void)ReplayMarketDataSource::from_csv(input);
        FAIL() << "Expected malformed input to fail";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("CSV line 2"), std::string::npos);
    }
}

TEST(Replay, RejectsIoFailureAndInvalidVector) {
    std::istringstream input(header);
    input.setstate(std::ios::badbit);
    EXPECT_THROW(ReplayMarketDataSource::from_csv(input), std::invalid_argument);
    auto data = sample_market_data();
    data[1].sequence = 1;
    EXPECT_THROW(ReplayMarketDataSource source(data), std::invalid_argument);
}

TEST(Engine, RunsDeterministicRoundTripAcrossThreads) {
    ReplayMarketDataSource source(sample_market_data());
    TradingEngine engine(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>(), {}, 1);
    engine.run(source);
    EXPECT_EQ(engine.processed_ticks(), 4U);
    ASSERT_EQ(engine.orders().orders().size(), 2U);
    ASSERT_EQ(engine.fills().size(), 2U);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Filled);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Filled);
    EXPECT_DOUBLE_EQ(engine.fills()[0].price, 101);
    EXPECT_DOUBLE_EQ(engine.fills()[1].price, 104);
    const auto position = engine.positions().get("SYNTH");
    EXPECT_EQ(position.quantity, 0);
    EXPECT_DOUBLE_EQ(position.realized_pnl, 6);
    EXPECT_DOUBLE_EQ(position.unrealized_pnl, 0);
    EXPECT_THROW(engine.run(source), std::logic_error);
}

TEST(Engine, RiskRejectionDoesNotReachExchangeOrPosition) {
    ReplayMarketDataSource source(sample_market_data());
    TradingEngine engine(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>(),
                         {1, 20, 10000, 100});
    engine.run(source);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Rejected);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Rejected);
    EXPECT_TRUE(engine.fills().empty());
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 0);
}

TEST(Engine, ReservesWorkingExposureAndCancelsPartialRemainderAtEof) {
    ReplayMarketDataSource source({{1, 1000, "SYNTH", 99, 101, 10, 10},
                                   {2, 2000, "SYNTH", 98, 100, 10, 1}});
    TradingEngine engine(std::make_unique<LimitStrategy>(), std::make_unique<SimulatedExchange>(),
                         {10, 5, 10000, 100});
    engine.run(source);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(1).filled_quantity, 1);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Rejected);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 1);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").unrealized_pnl, -1);
}

TEST(Engine, HandlesEmptyReplay) {
    ReplayMarketDataSource source({});
    TradingEngine engine(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>());
    engine.run(source);
    EXPECT_EQ(engine.processed_ticks(), 0U);
    EXPECT_TRUE(engine.orders().orders().empty());
}

TEST(Engine, PropagatesProducerFailureAndCancelsRestingOrders) {
    ThrowingSource source;
    TradingEngine engine(std::make_unique<LimitStrategy>(), std::make_unique<SimulatedExchange>());
    EXPECT_THROW(engine.run(source), std::runtime_error);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
}

TEST(Engine, ConsumerFailureUnblocksProducerAndJoins) {
    UncheckedSource source;
    TradingEngine engine(std::make_unique<ThrowingStrategy>(), std::make_unique<SimulatedExchange>(), {}, 1);
    EXPECT_THROW(engine.run(source), std::runtime_error);
}

TEST(Engine, RevalidatesCustomSourceOrdering) {
    UncheckedSource source;
    TradingEngine engine(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>(), {}, 1);
    EXPECT_THROW(engine.run(source), std::invalid_argument);
}

TEST(Strategy, OnlyActsOnFirstAndThirdMatchingTicks) {
    ExampleStrategy strategy;
    EXPECT_FALSE(strategy.on_market_data({1, 100, "OTHER", 1, 2, 10, 10}));
    auto ticks = sample_market_data();
    ASSERT_TRUE(strategy.on_market_data(ticks[0]));
    EXPECT_FALSE(strategy.on_market_data(ticks[1]));
    const auto sell = strategy.on_market_data(ticks[2]);
    ASSERT_TRUE(sell);
    EXPECT_EQ(sell->side, Side::Sell);
    EXPECT_FALSE(strategy.on_market_data(ticks[3]));
}
