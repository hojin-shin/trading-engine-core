#include "trading/simulated_exchange.hpp"
#include "trading/trading_engine.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <stdexcept>
using namespace trading;

namespace {
OrderId working(OrderManager& orders, Quantity quantity = 4) {
    const auto id = orders.create({"SYNTH", Side::Buy, quantity, OrderType::Limit, 101});
    orders.accept(id);
    return id;
}

// Replays all previously emitted reports on every tick and duplicates fresh reports
// in the same batch. Matching and liquidity remain those of the real simulator.
class RepeatingGateway final : public IExecutionGateway {
public:
    explicit RepeatingGateway(bool conflict = false) : conflict_(conflict) {}
    std::vector<Fill> on_market_data(const MarketData& data) override {
        auto fresh = exchange_.on_market_data(data);
        auto reports = history_;
        if (conflict_ && !reports.empty()) { reports.front().price += 0.5; }
        append(reports, fresh);
        return reports;
    }
    std::vector<Fill> submit(const Order& order) override {
        std::vector<Fill> reports;
        append(reports, exchange_.submit(order));
        return reports;
    }
    bool cancel(OrderId id) override { return exchange_.cancel(id); }
private:
    void append(std::vector<Fill>& reports, const std::vector<Fill>& fresh) {
        for (const auto& fill : fresh) {
            reports.push_back(fill);
            reports.push_back(fill);
            history_.push_back(fill);
        }
    }
    SimulatedExchange exchange_;
    std::vector<Fill> history_;
    bool conflict_;
};
}

TEST(ExecutionIds, DuplicatePartialAndFullFillsDoNotChangeOrder) {
    OrderManager orders;
    const auto id = working(orders, 2);
    const Fill first{id, "SYNTH", Side::Buy, 1, 100, 1000, 1};
    EXPECT_TRUE(orders.apply_fill(first));
    EXPECT_FALSE(orders.apply_fill(first));
    EXPECT_EQ(orders.get(id).filled_quantity, 1);
    auto second = first;
    second.execution_id = 2;
    EXPECT_TRUE(orders.apply_fill(second)); // Same economics and timestamp, distinct execution.
    EXPECT_EQ(orders.get(id).state, OrderState::Filled);
    EXPECT_FALSE(orders.apply_fill(first));
    EXPECT_FALSE(orders.apply_fill(second));
    EXPECT_DOUBLE_EQ(orders.get(id).average_fill_price, 100);
}

TEST(ExecutionIds, RetransmissionAfterCancellationIsIgnoredButNewFillFails) {
    OrderManager orders;
    const auto id = working(orders);
    Fill fill{id, "SYNTH", Side::Buy, 1, 100, 1000, 1};
    EXPECT_TRUE(orders.apply_fill(fill));
    orders.cancel(id);
    EXPECT_FALSE(orders.apply_fill(fill));
    fill.execution_id = 2;
    EXPECT_THROW(orders.apply_fill(fill), std::logic_error);
    EXPECT_EQ(orders.get(id).filled_quantity, 1);
    EXPECT_EQ(orders.get(id).state, OrderState::Cancelled);
}

TEST(ExecutionIds, EveryChangedFieldConflictsAndDoesNotMutateEitherOrder) {
    OrderManager orders;
    const auto id = working(orders);
    const auto other = working(orders);
    const Fill original{id, "SYNTH", Side::Buy, 1, 100, 1000, 1};
    ASSERT_TRUE(orders.apply_fill(original));
    for (int field = 0; field < 6; ++field) {
        SCOPED_TRACE(field);
        auto changed = original;
        switch (field) {
        case 0: changed.order_id = other; break;
        case 1: changed.symbol = "OTHER"; break;
        case 2: changed.side = Side::Sell; break;
        case 3: changed.quantity = 2; break;
        case 4: changed.price = 100.5; break;
        case 5: changed.timestamp_ns = 2000; break;
        }
        EXPECT_THROW(orders.apply_fill(changed), std::logic_error);
        EXPECT_EQ(orders.get(id).filled_quantity, 1);
        EXPECT_EQ(orders.get(other).filled_quantity, 0);
    }
    EXPECT_FALSE(orders.apply_fill(original));
}

TEST(ExecutionIds, InvalidReportsDoNotConsumeIds) {
    OrderManager orders;
    const auto id = working(orders);
    Fill fill{id, "SYNTH", Side::Buy, 1, 100, 1000, 0};
    EXPECT_THROW(orders.apply_fill(fill), std::invalid_argument);
    fill.execution_id = 1;
    fill.price = 102;
    EXPECT_THROW(orders.apply_fill(fill), std::logic_error);
    fill.price = 100;
    fill.order_id = 999;
    EXPECT_THROW(orders.apply_fill(fill), std::out_of_range);
    fill.order_id = id;
    EXPECT_TRUE(orders.apply_fill(fill));
    EXPECT_EQ(orders.get(id).filled_quantity, 1);
}

TEST(ExecutionIds, SimulatorAssignsDeterministicIdsAcrossOrdersAndPartialFills) {
    for (int run = 0; run < 2; ++run) {
        SimulatedExchange exchange;
        OrderManager orders;
        exchange.on_market_data({1, 1000, "SYNTH", 99, 101, 10, 1});
        const auto id = working(orders, 2);
        const auto first = exchange.submit(orders.get(id));
        ASSERT_EQ(first.size(), 1U);
        EXPECT_EQ(first[0].execution_id, 1U);
        const auto other = working(orders, 1);
        EXPECT_TRUE(exchange.submit(orders.get(other)).empty());
        const auto next = exchange.on_market_data({2, 2000, "SYNTH", 99, 101, 10, 2});
        ASSERT_EQ(next.size(), 2U);
        EXPECT_EQ(next[0].execution_id, 2U);
        EXPECT_EQ(next[1].execution_id, 3U);
    }
}

TEST(ExecutionIds, EngineRoundTripCountsOnlyUniqueFillsAndPnl) {
    ReplayMarketDataSource source(sample_market_data());
    std::ostringstream trace;
    TradingEngine engine(std::make_unique<ExampleStrategy>(),
                         std::make_unique<RepeatingGateway>(), {}, 1, &trace);
    ASSERT_NO_THROW(engine.run(source));
    ASSERT_EQ(engine.fills().size(), 2U);
    EXPECT_EQ(engine.orders().get(1).filled_quantity, 2);
    EXPECT_EQ(engine.orders().get(2).filled_quantity, 2);
    const auto position = engine.positions().get("SYNTH");
    EXPECT_EQ(position.quantity, 0);
    EXPECT_DOUBLE_EQ(position.realized_pnl, 6);
    EXPECT_DOUBLE_EQ(position.unrealized_pnl, 0);
    EXPECT_NE(trace.str().find("event=FILL_DUPLICATE order=1 execution_id=1"), std::string::npos);
    EXPECT_NE(trace.str().find("fill_timestamp_ns=1000 execution_id=1"), std::string::npos);
}

TEST(ExecutionIds, EngineIgnoresPartialFillReplaysAfterCancelAndReplace) {
    for (bool replace : {false, true}) {
        SCOPED_TRACE(replace);
        ReplayMarketDataSource source({{1, 1000, "SYNTH", 99, 100, 10, 1},
                                       {2, 2000, "SYNTH", 99, 101, 10, 1},
                                       {3, 3000, "SYNTH", 99, 101, 10, 10}});
        std::unique_ptr<IStrategy> strategy;
        if (replace) { strategy = std::make_unique<ReplaceExampleStrategy>(); }
        else { strategy = std::make_unique<CancelExampleStrategy>(); }
        TradingEngine engine(std::move(strategy), std::make_unique<RepeatingGateway>(), {}, 1);
        ASSERT_NO_THROW(engine.run(source));
        EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
        EXPECT_EQ(engine.orders().get(1).filled_quantity, 1);
        EXPECT_EQ(engine.fills().size(), replace ? 2U : 1U);
        const auto position = engine.positions().get("SYNTH");
        EXPECT_EQ(position.quantity, replace ? 2 : 1);
        EXPECT_DOUBLE_EQ(position.average_price, replace ? 100.5 : 100);
        EXPECT_DOUBLE_EQ(position.realized_pnl, 0);
        EXPECT_DOUBLE_EQ(position.unrealized_pnl, replace ? -1 : 0);
        if (replace) {
            EXPECT_EQ(engine.orders().get(2).filled_quantity, 1);
            EXPECT_EQ(engine.fills()[1].execution_id, 2U);
            ASSERT_EQ(engine.replace_results().size(), 1U);
            EXPECT_TRUE(engine.replace_results()[0].replaced);
        }
    }
}

TEST(ExecutionIds, ConflictingRetransmissionStopsEngineWithoutReapplyingPnl) {
    ReplayMarketDataSource source(sample_market_data());
    TradingEngine engine(std::make_unique<ExampleStrategy>(),
                         std::make_unique<RepeatingGateway>(true), {}, 1);
    EXPECT_THROW(engine.run(source), std::logic_error);
    ASSERT_EQ(engine.fills().size(), 1U);
    EXPECT_EQ(engine.orders().get(1).filled_quantity, 2);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 2);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").average_price, 101);
}
