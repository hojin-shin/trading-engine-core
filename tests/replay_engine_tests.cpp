#include "trading/simulated_exchange.hpp"
#include "trading/trading_engine.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

using namespace trading;

namespace {
constexpr auto header = "sequence,timestamp_ns,symbol,bid,ask,bid_size,ask_size\n";

class LimitStrategy final : public IStrategy {
public:
    std::optional<StrategyAction> on_market_data(const MarketData& data) override {
        if (data.sequence > 2) { return std::nullopt; }
        return Signal{data.symbol, Side::Buy, 4, OrderType::Limit, 100};
    }
};

class ThrowingStrategy final : public IStrategy {
public:
    std::optional<StrategyAction> on_market_data(const MarketData&) override {
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
    EXPECT_EQ(std::get<Signal>(*sell).side, Side::Sell);
    EXPECT_FALSE(strategy.on_market_data(ticks[3]));
}


TEST(Trace, PartialFillsAreChronologicalAndPreserveStreamFormatting) {
    auto ticks = sample_market_data();
    ticks.front().ask_size = 1;
    ReplayMarketDataSource source(ticks);
    std::ostringstream trace;
    trace.precision(7);
    const auto flags = trace.flags();
    TradingEngine engine(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>(),
                         {}, 1, &trace);
    engine.run(source);
    const auto output = trace.str();
    std::size_t cursor = 0;
    for (const auto* token : {"event=TICK", "state=PendingRisk", "state=Accepted",
                             "event=FILL order=1 symbol=SYNTH side=Buy quantity=1 price=101.00",
                             "state=PartiallyFilled", "event=POSITION cause=fill",
                             "seq=2 timestamp_ns=2000 event=TICK",
                             "event=FILL order=1 symbol=SYNTH side=Buy quantity=1 price=104.00",
                             "state=Filled", "average_price=102.50",
                             "seq=3 timestamp_ns=3000 event=TICK",
                             "event=FILL order=2 symbol=SYNTH side=Sell quantity=2 price=104.00"}) {
        const auto found = output.find(token, cursor);
        ASSERT_NE(found, std::string::npos) << token << '\n' << output;
        cursor = found + std::string_view(token).size();
    }
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").realized_pnl, 3);
    EXPECT_EQ(trace.precision(), 7);
    EXPECT_EQ(trace.flags(), flags);
}

TEST(Trace, EofCancellationRetainsPartialFillAndPosition) {
    ReplayMarketDataSource source({{1, 1000, "SYNTH", 99, 101, 10, 1}});
    std::ostringstream trace;
    TradingEngine engine(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>(),
                         {}, 1, &trace);
    engine.run(source);
    const auto output = trace.str();
    EXPECT_NE(output.find("state=Cancelled quantity=2 filled=1 unfilled=1 average_price=101.00 reason=end_of_replay"),
              std::string::npos);
    EXPECT_NE(output.find("event=POSITION cause=fill symbol=SYNTH quantity=1 average_price=101.00 mark_price=100.00 realized_pnl=0.00 unrealized_pnl=-1.00"),
              std::string::npos);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 1);
}

TEST(Trace, RejectedOrdersIncludeReasonWithoutFills) {
    ReplayMarketDataSource source(sample_market_data());
    std::ostringstream trace;
    TradingEngine engine(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>(),
                         {10, 20, 100, 1000}, 1, &trace);
    engine.run(source);
    const auto output = trace.str();
    EXPECT_NE(output.find("state=Rejected"), std::string::npos);
    EXPECT_NE(output.find("reason=\"Order notional limit\""), std::string::npos);
    EXPECT_EQ(output.find("event=FILL"), std::string::npos);
    EXPECT_TRUE(engine.fills().empty());
}

TEST(Trace, ErrorCleanupIsNotLabelledAsEndOfReplay) {
    ThrowingSource source;
    std::ostringstream trace;
    TradingEngine engine(std::make_unique<LimitStrategy>(), std::make_unique<SimulatedExchange>(),
                         {}, 1, &trace);
    EXPECT_THROW(engine.run(source), std::runtime_error);
    EXPECT_NE(trace.str().find("reason=run_error"), std::string::npos);
    EXPECT_EQ(trace.str().find("reason=end_of_replay"), std::string::npos);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
}

TEST(Trace, FailedOutputDoesNotPreventTradingOrCancellation) {
    for (const bool throw_on_failure : {false, true}) {
        ReplayMarketDataSource source({{1, 1000, "SYNTH", 99, 101, 10, 1}});
        std::ostream trace(nullptr);
        if (throw_on_failure) {
            EXPECT_THROW(trace.exceptions(std::ios::badbit), std::ios_base::failure);
        }
        TradingEngine engine(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>(),
                             {}, 1, &trace);
        EXPECT_NO_THROW(engine.run(source));
        EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
        EXPECT_EQ(engine.fills().size(), 1U);
        EXPECT_EQ(engine.positions().get("SYNTH").quantity, 1);
    }
}


TEST(Strategy, ValidatesBothExampleLimitPrices) {
    for (const auto invalid : {0.0, -1.0, max_price * 2,
                               std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_THROW((ExampleStrategy{"SYNTH", 2, ExampleLimitPrices{invalid, 104}}),
                     std::invalid_argument);
        EXPECT_THROW((ExampleStrategy{"SYNTH", 2, ExampleLimitPrices{100, invalid}}),
                     std::invalid_argument);
    }
}

TEST(Engine, ExampleLimitsWaitForExecutableQuotesOnBothSides) {
    ReplayMarketDataSource source({{1, 1000, "SYNTH", 99, 101, 10, 10},
                                   {2, 2000, "SYNTH", 98, 100, 10, 10},
                                   {3, 3000, "SYNTH", 103, 105, 10, 10},
                                   {4, 4000, "SYNTH", 104, 106, 10, 10}});
    TradingEngine engine(std::make_unique<ExampleStrategy>("SYNTH", 2, ExampleLimitPrices{100, 104}),
                         std::make_unique<SimulatedExchange>());
    engine.run(source);
    ASSERT_EQ(engine.fills().size(), 2U);
    EXPECT_EQ(engine.fills()[0].order_id, 1U);
    EXPECT_EQ(engine.fills()[0].timestamp_ns, 2000U);
    EXPECT_DOUBLE_EQ(engine.fills()[0].price, 100);
    EXPECT_EQ(engine.fills()[1].order_id, 2U);
    EXPECT_EQ(engine.fills()[1].timestamp_ns, 4000U);
    EXPECT_DOUBLE_EQ(engine.fills()[1].price, 104);
    EXPECT_EQ(engine.orders().get(1).request.type, OrderType::Limit);
    EXPECT_EQ(engine.orders().get(1).request.limit_price, 100);
    EXPECT_EQ(engine.orders().get(2).request.limit_price, 104);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Filled);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Filled);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 0);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").realized_pnl, 8);
}

TEST(Engine, ExampleLimitRemainsUnfilledWhenAskNeverReachesLimit) {
    ReplayMarketDataSource source({{1, 1000, "SYNTH", 99, 101, 10, 10},
                                   {2, 2000, "SYNTH", 100, 102, 10, 10}});
    TradingEngine engine(std::make_unique<ExampleStrategy>("SYNTH", 2, ExampleLimitPrices{100, 104}),
                         std::make_unique<SimulatedExchange>());
    engine.run(source);
    EXPECT_TRUE(engine.fills().empty());
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(1).filled_quantity, 0);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 0);
}

TEST(Engine, ExampleLimitPartialFillReceivesBetterPriceAndCancelsRemainder) {
    ReplayMarketDataSource source({{1, 1000, "SYNTH", 99, 101, 10, 10},
                                   {2, 2000, "SYNTH", 98, 99, 10, 1}});
    TradingEngine engine(std::make_unique<ExampleStrategy>("SYNTH", 2, ExampleLimitPrices{100, 104}),
                         std::make_unique<SimulatedExchange>());
    engine.run(source);
    ASSERT_EQ(engine.fills().size(), 1U);
    EXPECT_DOUBLE_EQ(engine.fills()[0].price, 99);
    EXPECT_EQ(engine.fills()[0].quantity, 1);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(1).filled_quantity, 1);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 1);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").unrealized_pnl, -0.5);
}
