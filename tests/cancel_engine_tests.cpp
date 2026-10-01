#include "trading/simulated_exchange.hpp"
#include "trading/trading_engine.hpp"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace trading;

namespace {

std::vector<MarketData> cancel_ticks() {
    return {{1, 1000, "SYNTH", 99, 101, 10, 10},
            {2, 2000, "SYNTH", 99, 101, 10, 10},
            {3, 3000, "SYNTH", 98, 100, 10, 10}};
}

class ProgramStrategy final : public IStrategy {
public:
    explicit ProgramStrategy(std::map<std::uint64_t, StrategyAction> actions)
        : actions_(std::move(actions)) {}
    std::optional<StrategyAction> on_market_data(const MarketData& data) override {
        const auto found = actions_.find(data.sequence);
        if (found == actions_.end()) { return std::nullopt; }
        return found->second;
    }
private:
    std::map<std::uint64_t, StrategyAction> actions_;
};

enum class CancelBehavior { Normal, DeclineOnce, ThrowOnce };

class ControlledGateway final : public IExecutionGateway {
public:
    explicit ControlledGateway(CancelBehavior behavior = CancelBehavior::Normal) : behavior_(behavior) {}
    std::vector<Fill> on_market_data(const MarketData& data) override { return exchange_.on_market_data(data); }
    std::vector<Fill> submit(const Order& order) override { return exchange_.submit(order); }
    bool cancel(OrderId id) override {
        ++cancel_calls;
        if (cancel_calls == 1 && behavior_ == CancelBehavior::DeclineOnce) { return false; }
        if (cancel_calls == 1 && behavior_ == CancelBehavior::ThrowOnce) {
            throw std::runtime_error("Cancellation transport failed");
        }
        return exchange_.cancel(id);
    }
    int cancel_calls{};
private:
    SimulatedExchange exchange_;
    CancelBehavior behavior_;
};

class ThrowingNotificationStrategy final : public IStrategy {
public:
    std::optional<StrategyAction> on_market_data(const MarketData& data) override {
        return Signal{data.symbol, Side::Buy, 2, OrderType::Limit, 100};
    }
    void on_order_created(const Order&) override { throw std::runtime_error("Notification failed"); }
};

} // namespace

TEST(Cancel, WorkingOrderCannotFillOnLaterExecutableQuote) {
    ReplayMarketDataSource source(cancel_ticks());
    std::ostringstream trace;
    TradingEngine engine(std::make_unique<CancelExampleStrategy>(), std::make_unique<SimulatedExchange>(),
                         {}, 1, &trace);
    engine.run(source);
    EXPECT_EQ(engine.processed_ticks(), 3U);
    ASSERT_EQ(engine.cancel_results().size(), 1U);
    EXPECT_TRUE(engine.cancel_results()[0].cancelled);
    EXPECT_EQ(engine.cancel_results()[0].order_id, 1U);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_TRUE(engine.fills().empty());
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 0);
    const auto output = trace.str();
    const auto request = output.find("seq=2 timestamp_ns=2000 event=CANCEL_REQUEST order=1");
    const auto cancelled = output.find("state=Cancelled");
    const auto later_quote = output.find("seq=3 timestamp_ns=3000 event=TICK");
    ASSERT_NE(request, std::string::npos);
    ASSERT_NE(cancelled, std::string::npos);
    ASSERT_NE(later_quote, std::string::npos);
    EXPECT_LT(request, cancelled);
    EXPECT_LT(cancelled, later_quote);
    EXPECT_NE(output.find("reason=strategy_request"), std::string::npos);
    EXPECT_EQ(output.find("event=FILL"), std::string::npos);
    EXPECT_EQ(output.find("reason=end_of_replay"), std::string::npos);
}

TEST(Cancel, PartialFillRemainsInPositionAfterRemainderIsCancelled) {
    auto ticks = cancel_ticks();
    ticks[0] = {1, 1000, "SYNTH", 98, 100, 10, 1};
    ReplayMarketDataSource source(ticks);
    TradingEngine engine(std::make_unique<CancelExampleStrategy>(), std::make_unique<SimulatedExchange>());
    engine.run(source);
    ASSERT_EQ(engine.fills().size(), 1U);
    EXPECT_EQ(engine.fills()[0].quantity, 1);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(1).remaining(), 1);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 1);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").average_price, 100);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").realized_pnl, 0);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").unrealized_pnl, -1);
}

TEST(Cancel, FullFillOnRequestTickWinsBeforeCancellation) {
    auto ticks = cancel_ticks();
    ticks[1] = {2, 2000, "SYNTH", 98, 100, 10, 10};
    ReplayMarketDataSource source(ticks);
    auto gateway = std::make_unique<ControlledGateway>();
    const auto* observed = gateway.get();
    TradingEngine engine(std::make_unique<CancelExampleStrategy>(), std::move(gateway));
    engine.run(source);
    ASSERT_EQ(engine.cancel_results().size(), 1U);
    EXPECT_FALSE(engine.cancel_results()[0].cancelled);
    EXPECT_EQ(engine.cancel_results()[0].reason, "Order is not working");
    EXPECT_EQ(observed->cancel_calls, 0);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Filled);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 2);
    ASSERT_EQ(engine.fills().size(), 1U);
    EXPECT_EQ(engine.fills()[0].timestamp_ns, 2000U);
}

TEST(Cancel, PartialFillOnRequestTickLeavesOnlyRemainderToCancel) {
    auto ticks = cancel_ticks();
    ticks[1] = {2, 2000, "SYNTH", 98, 100, 10, 1};
    ReplayMarketDataSource source(ticks);
    TradingEngine engine(std::make_unique<CancelExampleStrategy>(), std::make_unique<SimulatedExchange>());
    engine.run(source);
    ASSERT_EQ(engine.fills().size(), 1U);
    EXPECT_EQ(engine.fills()[0].timestamp_ns, 2000U);
    ASSERT_EQ(engine.cancel_results().size(), 1U);
    EXPECT_TRUE(engine.cancel_results()[0].cancelled);
    EXPECT_EQ(engine.orders().get(1).filled_quantity, 1);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
}

TEST(Cancel, UnknownIdsAreRejectedWithoutStoppingLaterTrading) {
    auto strategy = std::make_unique<ProgramStrategy>(std::map<std::uint64_t, StrategyAction>{
        {1, CancelRequest{0}}, {2, CancelRequest{999}},
        {3, Signal{"SYNTH", Side::Buy, 2, OrderType::Market, std::nullopt}}});
    ReplayMarketDataSource source(cancel_ticks());
    auto gateway = std::make_unique<ControlledGateway>();
    const auto* observed = gateway.get();
    TradingEngine engine(std::move(strategy), std::move(gateway));
    EXPECT_NO_THROW(engine.run(source));
    ASSERT_EQ(engine.cancel_results().size(), 2U);
    for (const auto& result : engine.cancel_results()) {
        EXPECT_FALSE(result.cancelled);
        EXPECT_EQ(result.reason, "Unknown order");
    }
    EXPECT_EQ(observed->cancel_calls, 0);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Filled);
}

TEST(Cancel, RepeatedRequestDoesNotCancelAnAlreadyCancelledOrderAgain) {
    auto strategy = std::make_unique<ProgramStrategy>(std::map<std::uint64_t, StrategyAction>{
        {1, Signal{"SYNTH", Side::Buy, 2, OrderType::Limit, 100}},
        {2, CancelRequest{1}}, {3, CancelRequest{1}}});
    ReplayMarketDataSource source(cancel_ticks());
    auto gateway = std::make_unique<ControlledGateway>();
    const auto* observed = gateway.get();
    TradingEngine engine(std::move(strategy), std::move(gateway));
    engine.run(source);
    ASSERT_EQ(engine.cancel_results().size(), 2U);
    EXPECT_TRUE(engine.cancel_results()[0].cancelled);
    EXPECT_FALSE(engine.cancel_results()[1].cancelled);
    EXPECT_EQ(engine.cancel_results()[1].reason, "Order is not working");
    EXPECT_EQ(observed->cancel_calls, 1);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_TRUE(engine.fills().empty());
}

TEST(Cancel, RiskRejectedOrderReceivesIdButCannotBeCancelled) {
    ReplayMarketDataSource source(cancel_ticks());
    auto gateway = std::make_unique<ControlledGateway>();
    const auto* observed = gateway.get();
    TradingEngine engine(std::make_unique<CancelExampleStrategy>(), std::move(gateway), {1, 20, 10000, 1000});
    engine.run(source);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Rejected);
    ASSERT_EQ(engine.cancel_results().size(), 1U);
    EXPECT_EQ(engine.cancel_results()[0].order_id, 1U);
    EXPECT_FALSE(engine.cancel_results()[0].cancelled);
    EXPECT_EQ(observed->cancel_calls, 0);
}

TEST(Cancel, LossLimitDoesNotBlockCancellation) {
    auto ticks = cancel_ticks();
    ticks[0] = {1, 1000, "SYNTH", 98, 100, 10, 1};
    ticks[1] = {2, 2000, "SYNTH", 96, 98, 10, 0};
    ReplayMarketDataSource source(ticks);
    TradingEngine engine(std::make_unique<CancelExampleStrategy>(), std::make_unique<SimulatedExchange>(),
                         {10, 20, 10000, 1});
    engine.run(source);
    ASSERT_EQ(engine.cancel_results().size(), 1U);
    EXPECT_TRUE(engine.cancel_results()[0].cancelled);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 1);
}

TEST(Cancel, SuccessfulCancellationReleasesWorkingRiskReservation) {
    auto strategy = std::make_unique<ProgramStrategy>(std::map<std::uint64_t, StrategyAction>{
        {1, Signal{"SYNTH", Side::Buy, 2, OrderType::Limit, 100}},
        {2, CancelRequest{1}},
        {3, Signal{"SYNTH", Side::Buy, 2, OrderType::Limit, 100}}});
    ReplayMarketDataSource source(cancel_ticks());
    TradingEngine engine(std::move(strategy), std::make_unique<SimulatedExchange>(), {10, 2, 10000, 1000});
    engine.run(source);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Filled);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 2);
}

TEST(Cancel, GatewayDeclineLeavesOrderWorkingAndItCanFillLater) {
    ReplayMarketDataSource source(cancel_ticks());
    auto gateway = std::make_unique<ControlledGateway>(CancelBehavior::DeclineOnce);
    const auto* observed = gateway.get();
    TradingEngine engine(std::make_unique<CancelExampleStrategy>(), std::move(gateway));
    engine.run(source);
    ASSERT_EQ(engine.cancel_results().size(), 1U);
    EXPECT_FALSE(engine.cancel_results()[0].cancelled);
    EXPECT_EQ(engine.cancel_results()[0].reason, "Gateway declined cancellation");
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Filled);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 2);
    EXPECT_EQ(observed->cancel_calls, 1);
}

TEST(Cancel, GatewayExceptionSurfacesAndCleanupStillRuns) {
    ReplayMarketDataSource source(cancel_ticks());
    auto gateway = std::make_unique<ControlledGateway>(CancelBehavior::ThrowOnce);
    const auto* observed = gateway.get();
    std::ostringstream trace;
    TradingEngine engine(std::make_unique<CancelExampleStrategy>(), std::move(gateway), {}, 1, &trace);
    try {
        engine.run(source);
        FAIL() << "Expected gateway exception";
    } catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "Cancellation transport failed");
    }
    EXPECT_EQ(observed->cancel_calls, 2);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_TRUE(engine.cancel_results().empty());
    EXPECT_NE(trace.str().find("reason=run_error"), std::string::npos);
}

TEST(Cancel, NotificationFailureCancelsSubmittedOrderDuringCleanup) {
    ReplayMarketDataSource source(cancel_ticks());
    TradingEngine engine(std::make_unique<ThrowingNotificationStrategy>(), std::make_unique<SimulatedExchange>(), {}, 1);
    EXPECT_THROW(engine.run(source), std::runtime_error);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_TRUE(engine.fills().empty());
}

TEST(Cancel, ExampleUsesProvidedOrderIdAndCountsOnlyMatchingSymbols) {
    CancelExampleStrategy strategy;
    EXPECT_FALSE(strategy.on_market_data({1, 1000, "OTHER", 99, 101, 10, 10}));
    const auto submit = strategy.on_market_data({2, 2000, "SYNTH", 99, 101, 10, 10});
    ASSERT_TRUE(submit);
    ASSERT_TRUE(std::holds_alternative<Signal>(*submit));
    strategy.on_order_created({73, std::get<Signal>(*submit), OrderState::Accepted, 0, 0, {}});
    EXPECT_FALSE(strategy.on_market_data({3, 3000, "OTHER", 99, 101, 10, 10}));
    const auto cancel = strategy.on_market_data({4, 4000, "SYNTH", 99, 101, 10, 10});
    ASSERT_TRUE(cancel);
    ASSERT_TRUE(std::holds_alternative<CancelRequest>(*cancel));
    EXPECT_EQ(std::get<CancelRequest>(*cancel).order_id, 73U);
    EXPECT_FALSE(strategy.on_market_data({5, 5000, "SYNTH", 99, 101, 10, 10}));
}
