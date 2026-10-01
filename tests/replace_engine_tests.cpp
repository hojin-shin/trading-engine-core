#include "trading/simulated_exchange.hpp"
#include "trading/trading_engine.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace trading;

namespace {

std::vector<MarketData> replace_ticks() {
    return {{1, 1000, "SYNTH", 99, 101, 10, 10},
            {2, 2000, "SYNTH", 99, 101, 10, 10},
            {3, 3000, "SYNTH", 100, 100, 10, 10}};
}

class Script final : public IStrategy {
public:
    explicit Script(std::map<std::uint64_t, StrategyAction> actions) : actions_(std::move(actions)) {}
    std::optional<StrategyAction> on_market_data(const MarketData& data) override {
        const auto found = actions_.find(data.sequence);
        if (found == actions_.end()) { return std::nullopt; }
        return found->second;
    }
    void on_order_created(const Order& order) override { notifications.push_back(order); }
    std::vector<Order> notifications;
private:
    std::map<std::uint64_t, StrategyAction> actions_;
};

enum class CancelBehavior { Normal, DeclineOnce, ThrowOnce };

class Gateway final : public IExecutionGateway {
public:
    explicit Gateway(CancelBehavior behavior = CancelBehavior::Normal) : behavior_(behavior) {}
    std::vector<Fill> on_market_data(const MarketData& data) override { return exchange_.on_market_data(data); }
    std::vector<Fill> submit(const Order& order) override {
        ++submit_calls;
        return exchange_.submit(order);
    }
    bool cancel(OrderId id) override {
        ++cancel_calls;
        if (cancel_calls == 1 && behavior_ == CancelBehavior::DeclineOnce) { return false; }
        if (cancel_calls == 1 && behavior_ == CancelBehavior::ThrowOnce) {
            throw std::runtime_error("Cancellation transport failed");
        }
        return exchange_.cancel(id);
    }
    int submit_calls{};
    int cancel_calls{};
private:
    SimulatedExchange exchange_;
    CancelBehavior behavior_;
};

std::unique_ptr<Script> script(double new_price = 101) {
    return std::make_unique<Script>(std::map<std::uint64_t, StrategyAction>{
        {1, Signal{"SYNTH", Side::Buy, 2, OrderType::Limit, 100}}, {2, ReplaceRequest{1, new_price}}});
}

} // namespace

TEST(Replace, CancelsOriginalBeforeNewRiskAndFillsWithLinkedIds) {
    ReplayMarketDataSource source(replace_ticks());
    std::ostringstream trace;
    auto strategy = script();
    const auto* observed = strategy.get();
    TradingEngine engine(std::move(strategy), std::make_unique<SimulatedExchange>(), {}, 1, &trace);
    engine.run(source);
    ASSERT_EQ(engine.orders().orders().size(), 2U);
    const auto& original = engine.orders().get(1);
    EXPECT_EQ(original.state, OrderState::Cancelled);
    EXPECT_EQ(original.filled_quantity, 0);
    EXPECT_DOUBLE_EQ(*original.request.limit_price, 100);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Filled);
    EXPECT_DOUBLE_EQ(*engine.orders().get(2).request.limit_price, 101);
    ASSERT_EQ(engine.fills().size(), 1U);
    EXPECT_EQ(engine.fills()[0].order_id, 2U);
    EXPECT_EQ(engine.fills()[0].quantity, 2);
    EXPECT_EQ(engine.fills()[0].timestamp_ns, 2000U);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 2);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").unrealized_pnl, -2);
    ASSERT_EQ(engine.replace_results().size(), 1U);
    const auto& result = engine.replace_results()[0];
    EXPECT_TRUE(result.original_cancelled);
    EXPECT_TRUE(result.replaced);
    EXPECT_EQ(result.order_id, 1U);
    EXPECT_EQ(result.replacement_order_id, 2U);
    EXPECT_TRUE(engine.cancel_results().empty());
    ASSERT_EQ(observed->notifications.size(), 2U);
    EXPECT_EQ(observed->notifications[1].id, 2U);
    const auto output = trace.str();
    const auto request = output.find("event=REPLACE_REQUEST order=1 limit_price=101.00");
    const auto cancelled = output.find("state=Cancelled");
    const auto pending = output.find("order=2 symbol=SYNTH side=Buy type=Limit state=PendingRisk");
    const auto fill = output.find("event=FILL order=2");
    const auto completed = output.find("event=REPLACE_RESULT order=1 original_cancelled=true replacement_order=2 result=Replaced");
    ASSERT_NE(request, std::string::npos);
    ASSERT_NE(cancelled, std::string::npos);
    ASSERT_NE(pending, std::string::npos);
    ASSERT_NE(fill, std::string::npos);
    ASSERT_NE(completed, std::string::npos);
    EXPECT_LT(request, cancelled);
    EXPECT_LT(cancelled, pending);
    EXPECT_LT(pending, fill);
    EXPECT_LT(fill, completed);
}

TEST(Replace, EarlierPartialFillIsNotSubmittedAgain) {
    auto ticks = replace_ticks();
    ticks[0] = {1, 1000, "SYNTH", 98, 100, 10, 1};
    ReplayMarketDataSource source(ticks);
    TradingEngine engine(std::make_unique<ReplaceExampleStrategy>(), std::make_unique<SimulatedExchange>(),
                         {10, 2, 10000, 1000});
    engine.run(source);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(1).request.quantity, 2);
    EXPECT_EQ(engine.orders().get(1).filled_quantity, 1);
    EXPECT_EQ(engine.orders().get(2).request.quantity, 1);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Filled);
    ASSERT_EQ(engine.fills().size(), 2U);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 2);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").average_price, 100.5);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").realized_pnl, 0);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").unrealized_pnl, -1);
}

TEST(Replace, SameTickPartialFillReducesRemainderAndDoesNotRefreshLiquidity) {
    auto ticks = replace_ticks();
    ticks[1] = {2, 2000, "SYNTH", 98, 100, 10, 1};
    ReplayMarketDataSource source(ticks);
    TradingEngine engine(script(), std::make_unique<SimulatedExchange>());
    engine.run(source);
    EXPECT_EQ(engine.orders().get(1).filled_quantity, 1);
    EXPECT_EQ(engine.orders().get(2).request.quantity, 1);
    ASSERT_EQ(engine.fills().size(), 2U);
    EXPECT_EQ(engine.fills()[0].order_id, 1U);
    EXPECT_EQ(engine.fills()[0].timestamp_ns, 2000U);
    EXPECT_EQ(engine.fills()[1].order_id, 2U);
    EXPECT_EQ(engine.fills()[1].timestamp_ns, 3000U);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 2);
}

TEST(Replace, SameTickFullFillRejectsReplacementWithoutGatewayCancellation) {
    auto ticks = replace_ticks();
    ticks[1].ask = 100;
    ReplayMarketDataSource source(ticks);
    auto gateway = std::make_unique<Gateway>();
    const auto* observed = gateway.get();
    TradingEngine engine(script(), std::move(gateway));
    engine.run(source);
    ASSERT_EQ(engine.replace_results().size(), 1U);
    EXPECT_EQ(engine.replace_results()[0].reason, "Order is not working");
    EXPECT_FALSE(engine.replace_results()[0].original_cancelled);
    EXPECT_FALSE(engine.replace_results()[0].replacement_order_id);
    EXPECT_EQ(observed->cancel_calls, 0);
    EXPECT_EQ(observed->submit_calls, 1);
    EXPECT_EQ(engine.orders().orders().size(), 1U);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Filled);
}

TEST(Replace, GatewayDeclineKeepsOriginalAndSubmitsNothingNew) {
    ReplayMarketDataSource source(replace_ticks());
    auto gateway = std::make_unique<Gateway>(CancelBehavior::DeclineOnce);
    const auto* observed = gateway.get();
    TradingEngine engine(script(), std::move(gateway));
    engine.run(source);
    ASSERT_EQ(engine.replace_results().size(), 1U);
    EXPECT_EQ(engine.replace_results()[0].reason, "Gateway declined cancellation");
    EXPECT_FALSE(engine.replace_results()[0].original_cancelled);
    EXPECT_FALSE(engine.replace_results()[0].replaced);
    EXPECT_FALSE(engine.replace_results()[0].replacement_order_id);
    EXPECT_EQ(observed->submit_calls, 1);
    EXPECT_EQ(observed->cancel_calls, 1);
    EXPECT_EQ(engine.orders().orders().size(), 1U);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Filled);
    ASSERT_EQ(engine.fills().size(), 1U);
    EXPECT_EQ(engine.fills()[0].timestamp_ns, 3000U);
}

TEST(Replace, NewNotionalRiskRejectionLeavesOriginalCancelled) {
    ReplayMarketDataSource source(replace_ticks());
    auto gateway = std::make_unique<Gateway>();
    const auto* observed_gateway = gateway.get();
    auto strategy = script();
    const auto* observed_strategy = strategy.get();
    TradingEngine engine(std::move(strategy), std::move(gateway), {10, 20, 200, 1000});
    engine.run(source);
    ASSERT_EQ(engine.replace_results().size(), 1U);
    const auto& result = engine.replace_results()[0];
    EXPECT_TRUE(result.original_cancelled);
    EXPECT_FALSE(result.replaced);
    EXPECT_EQ(result.replacement_order_id, 2U);
    EXPECT_EQ(result.reason, "Order notional limit");
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Rejected);
    EXPECT_TRUE(engine.fills().empty());
    EXPECT_EQ(observed_gateway->submit_calls, 1);
    ASSERT_EQ(observed_strategy->notifications.size(), 2U);
    EXPECT_EQ(observed_strategy->notifications[1].state, OrderState::Rejected);
}

TEST(Replace, LossGateRejectsNewOrderButDoesNotBlockOriginalCancellation) {
    auto ticks = replace_ticks();
    ticks[0] = {1, 1000, "SYNTH", 98, 100, 10, 1};
    ticks[1] = {2, 2000, "SYNTH", 94, 96, 10, 0};
    ReplayMarketDataSource source(ticks);
    TradingEngine engine(script(), std::make_unique<SimulatedExchange>(), {10, 20, 10000, 2});
    engine.run(source);
    ASSERT_EQ(engine.replace_results().size(), 1U);
    EXPECT_TRUE(engine.replace_results()[0].original_cancelled);
    EXPECT_FALSE(engine.replace_results()[0].replaced);
    EXPECT_EQ(engine.replace_results()[0].reason, "Session loss limit");
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Rejected);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 1);
    EXPECT_EQ(engine.fills().size(), 1U);
}

TEST(Replace, OriginalReservationIsReleasedBeforeCheckingReplacement) {
    ReplayMarketDataSource source(replace_ticks());
    TradingEngine engine(script(), std::make_unique<SimulatedExchange>(), {10, 2, 10000, 1000});
    engine.run(source);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Filled);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 2);
}

TEST(Replace, InvalidPricesLeaveOriginalWorkingUntilItFillsLater) {
    for (double price : {0.0, -1.0, max_price * 2, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
        SCOPED_TRACE(price);
        ReplayMarketDataSource source(replace_ticks());
        auto gateway = std::make_unique<Gateway>();
        const auto* observed = gateway.get();
        TradingEngine engine(script(price), std::move(gateway));
        engine.run(source);
        ASSERT_EQ(engine.replace_results().size(), 1U);
        EXPECT_EQ(engine.replace_results()[0].reason, "Invalid replacement price");
        EXPECT_FALSE(engine.replace_results()[0].original_cancelled);
        EXPECT_EQ(observed->cancel_calls, 0);
        EXPECT_EQ(observed->submit_calls, 1);
        EXPECT_EQ(engine.orders().get(1).state, OrderState::Filled);
    }
}

TEST(Replace, UnknownOrderDoesNotCreateNewOrder) {
    auto strategy = std::make_unique<Script>(std::map<std::uint64_t, StrategyAction>{
        {1, ReplaceRequest{0, 101}}, {2, ReplaceRequest{999, 101}}});
    ReplayMarketDataSource source(replace_ticks());
    TradingEngine engine(std::move(strategy), std::make_unique<SimulatedExchange>());
    engine.run(source);
    ASSERT_EQ(engine.replace_results().size(), 2U);
    for (const auto& result : engine.replace_results()) {
        EXPECT_EQ(result.reason, "Unknown order");
        EXPECT_FALSE(result.original_cancelled);
        EXPECT_FALSE(result.replacement_order_id);
    }
    EXPECT_TRUE(engine.orders().orders().empty());
}

TEST(Replace, RepeatedRequestForOriginalDoesNotDuplicateReplacement) {
    auto strategy = std::make_unique<Script>(std::map<std::uint64_t, StrategyAction>{
        {1, Signal{"SYNTH", Side::Buy, 2, OrderType::Limit, 100}},
        {2, ReplaceRequest{1, 101}}, {3, ReplaceRequest{1, 102}}});
    ReplayMarketDataSource source(replace_ticks());
    TradingEngine engine(std::move(strategy), std::make_unique<SimulatedExchange>());
    engine.run(source);
    ASSERT_EQ(engine.replace_results().size(), 2U);
    EXPECT_TRUE(engine.replace_results()[0].replaced);
    EXPECT_FALSE(engine.replace_results()[1].replaced);
    EXPECT_EQ(engine.replace_results()[1].reason, "Order is not working");
    EXPECT_EQ(engine.orders().orders().size(), 2U);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 2);
}

TEST(Replace, RiskRejectedOriginalCannotBeReplaced) {
    ReplayMarketDataSource source(replace_ticks());
    TradingEngine engine(std::make_unique<ReplaceExampleStrategy>(), std::make_unique<SimulatedExchange>(),
                         {1, 20, 10000, 1000});
    engine.run(source);
    ASSERT_EQ(engine.replace_results().size(), 1U);
    EXPECT_EQ(engine.replace_results()[0].reason, "Order is not working");
    EXPECT_EQ(engine.orders().orders().size(), 1U);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Rejected);
}

TEST(Replace, MarketOrderRemainderCannotBePriceAmended) {
    auto strategy = std::make_unique<Script>(std::map<std::uint64_t, StrategyAction>{
        {1, Signal{"SYNTH", Side::Buy, 2, OrderType::Market, std::nullopt}}, {2, ReplaceRequest{1, 101}}});
    auto ticks = replace_ticks();
    ticks[0].ask_size = 0;
    ticks[1].ask_size = 0;
    ReplayMarketDataSource source(ticks);
    TradingEngine engine(std::move(strategy), std::make_unique<SimulatedExchange>());
    engine.run(source);
    ASSERT_EQ(engine.replace_results().size(), 1U);
    EXPECT_EQ(engine.replace_results()[0].reason, "Replacement requires a limit order");
    EXPECT_FALSE(engine.replace_results()[0].original_cancelled);
    EXPECT_EQ(engine.orders().orders().size(), 1U);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Filled);
}

TEST(Replace, OtherSymbolTickCannotCancelOriginalForReplacement) {
    auto ticks = replace_ticks();
    ticks[1].symbol = "OTHER";
    ReplayMarketDataSource source(ticks);
    TradingEngine engine(script(), std::make_unique<SimulatedExchange>());
    engine.run(source);
    ASSERT_EQ(engine.replace_results().size(), 1U);
    EXPECT_EQ(engine.replace_results()[0].reason, "No matching current quote");
    EXPECT_FALSE(engine.replace_results()[0].original_cancelled);
    EXPECT_EQ(engine.orders().orders().size(), 1U);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Filled);
}

TEST(Replace, SellReplacementPreservesSideAndCanFillLater) {
    auto strategy = std::make_unique<Script>(std::map<std::uint64_t, StrategyAction>{
        {1, Signal{"SYNTH", Side::Sell, 2, OrderType::Limit, 102}}, {2, ReplaceRequest{1, 100}}});
    ReplayMarketDataSource source(replace_ticks());
    TradingEngine engine(std::move(strategy), std::make_unique<SimulatedExchange>());
    engine.run(source);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(2).request.side, Side::Sell);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Filled);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, -2);
    ASSERT_EQ(engine.fills().size(), 1U);
    EXPECT_EQ(engine.fills()[0].timestamp_ns, 3000U);
}

TEST(Replace, UnfilledReplacementIsCancelledAtEndOfReplay) {
    ReplayMarketDataSource source(replace_ticks());
    TradingEngine engine(script(98), std::make_unique<SimulatedExchange>());
    engine.run(source);
    ASSERT_EQ(engine.replace_results().size(), 1U);
    EXPECT_TRUE(engine.replace_results()[0].replaced);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Cancelled);
    EXPECT_TRUE(engine.fills().empty());
}

TEST(Replace, GatewayExceptionStopsReplacementAndCleanupCancelsOriginal) {
    ReplayMarketDataSource source(replace_ticks());
    auto gateway = std::make_unique<Gateway>(CancelBehavior::ThrowOnce);
    const auto* observed = gateway.get();
    TradingEngine engine(script(), std::move(gateway), {}, 1);
    EXPECT_THROW(engine.run(source), std::runtime_error);
    EXPECT_TRUE(engine.replace_results().empty());
    EXPECT_EQ(observed->submit_calls, 1);
    EXPECT_EQ(observed->cancel_calls, 2);
    EXPECT_EQ(engine.orders().orders().size(), 1U);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
}

TEST(Replace, ExampleUsesAssignedIdCountsMatchingTicksAndValidatesPrices) {
    ReplaceExampleStrategy strategy;
    EXPECT_FALSE(strategy.on_market_data({1, 1000, "OTHER", 99, 101, 10, 10}));
    auto action = strategy.on_market_data({2, 2000, "SYNTH", 99, 101, 10, 10});
    ASSERT_TRUE(action);
    ASSERT_TRUE(std::holds_alternative<Signal>(*action));
    strategy.on_order_created({73, std::get<Signal>(*action), OrderState::Accepted, 0, 0, {}});
    EXPECT_FALSE(strategy.on_market_data({3, 3000, "OTHER", 99, 101, 10, 10}));
    action = strategy.on_market_data({4, 4000, "SYNTH", 99, 101, 10, 10});
    ASSERT_TRUE(action);
    ASSERT_TRUE(std::holds_alternative<ReplaceRequest>(*action));
    EXPECT_EQ(std::get<ReplaceRequest>(*action).order_id, 73U);
    EXPECT_DOUBLE_EQ(std::get<ReplaceRequest>(*action).limit_price, 101);
    EXPECT_FALSE(strategy.on_market_data({5, 5000, "SYNTH", 99, 101, 10, 10}));
    EXPECT_THROW((ReplaceExampleStrategy{"SYNTH", 2, 0, 101}), std::invalid_argument);
    EXPECT_THROW((ReplaceExampleStrategy{"SYNTH", 2, 100, -1}), std::invalid_argument);
}
