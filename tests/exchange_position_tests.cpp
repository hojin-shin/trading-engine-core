#include "trading/order_manager.hpp"
#include "trading/position_manager.hpp"
#include "trading/simulated_exchange.hpp"

#include <gtest/gtest.h>

using namespace trading;

namespace {
Order accepted(OrderId id, Side side, Quantity quantity, std::optional<double> limit = {}) {
    return {id, {"SYNTH", side, quantity, limit ? OrderType::Limit : OrderType::Market, limit},
            OrderState::Accepted, 0, 0, {}};
}
} // namespace

TEST(Exchange, MarketBuysAtAskAndSellsAtBid) {
    SimulatedExchange exchange;
    EXPECT_TRUE(exchange.on_market_data({1, 1000, "SYNTH", 99, 101, 10, 10}).empty());
    const auto buy = exchange.submit(accepted(1, Side::Buy, 2));
    const auto sell = exchange.submit(accepted(2, Side::Sell, 3));
    ASSERT_EQ(buy.size(), 1U);
    ASSERT_EQ(sell.size(), 1U);
    EXPECT_DOUBLE_EQ(buy.front().price, 101);
    EXPECT_DOUBLE_EQ(sell.front().price, 99);
    EXPECT_EQ(buy.front().timestamp_ns, 1000U);
    EXPECT_FALSE(exchange.cancel(1));
}

TEST(Exchange, SharesLiquidityAndFillsRemaindersOnLaterTicks) {
    SimulatedExchange exchange;
    exchange.on_market_data({1, 1000, "SYNTH", 99, 101, 0, 2});
    const auto first = exchange.submit(accepted(1, Side::Buy, 3));
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(first.front().quantity, 2);
    EXPECT_TRUE(exchange.submit(accepted(2, Side::Buy, 2)).empty());
    const auto second = exchange.on_market_data({2, 2000, "SYNTH", 100, 102, 5, 2});
    ASSERT_EQ(second.size(), 2U);
    EXPECT_EQ(second[0].order_id, 1U);
    EXPECT_EQ(second[0].quantity, 1);
    EXPECT_EQ(second[1].order_id, 2U);
    EXPECT_EQ(second[1].quantity, 1);
    EXPECT_TRUE(exchange.cancel(2));
    EXPECT_TRUE(exchange.on_market_data({3, 3000, "SYNTH", 100, 102, 10, 10}).empty());
}

TEST(Exchange, RestingLimitsWaitForCrossAndReceivePriceImprovement) {
    SimulatedExchange exchange;
    exchange.on_market_data({1, 1000, "SYNTH", 99, 101, 10, 10});
    EXPECT_TRUE(exchange.submit(accepted(1, Side::Buy, 2, 100)).empty());
    EXPECT_TRUE(exchange.submit(accepted(2, Side::Sell, 2, 103)).empty());
    const auto buy = exchange.on_market_data({2, 2000, "SYNTH", 98, 99, 10, 10});
    ASSERT_EQ(buy.size(), 1U);
    EXPECT_EQ(buy.front().order_id, 1U);
    EXPECT_DOUBLE_EQ(buy.front().price, 99);
    const auto sell = exchange.on_market_data({3, 3000, "SYNTH", 104, 105, 10, 10});
    ASSERT_EQ(sell.size(), 1U);
    EXPECT_EQ(sell.front().order_id, 2U);
    EXPECT_DOUBLE_EQ(sell.front().price, 104);
}

TEST(Exchange, RejectsMissingQuoteDuplicateIdAndStaleTicks) {
    SimulatedExchange exchange;
    const auto order = accepted(1, Side::Buy, 2);
    EXPECT_THROW(exchange.submit(order), std::invalid_argument);
    const MarketData tick{1, 1000, "SYNTH", 99, 101, 10, 10};
    exchange.on_market_data(tick);
    exchange.submit(order);
    EXPECT_THROW(exchange.submit(order), std::invalid_argument);
    EXPECT_THROW(exchange.on_market_data(tick), std::invalid_argument);
    EXPECT_THROW(exchange.on_market_data(MarketData{2, 999, "SYNTH", 99, 101, 10, 10}),
                 std::invalid_argument);
}

TEST(Exchange, OtherSymbolsDoNotFillOrRefreshLiquidity) {
    SimulatedExchange exchange;
    exchange.on_market_data({1, 1000, "SYNTH", 99, 101, 10, 1});
    exchange.submit(accepted(1, Side::Buy, 2));
    EXPECT_TRUE(exchange.on_market_data({2, 2000, "OTHER", 1, 2, 100, 100}).empty());
    EXPECT_TRUE(exchange.submit(accepted(2, Side::Buy, 1)).empty());
}

TEST(Positions, WeightedAveragePartialCloseAndMarkToMarket) {
    PositionManager positions;
    positions.apply_fill({1, "SYNTH", Side::Buy, 2, 100, 1});
    positions.apply_fill({2, "SYNTH", Side::Buy, 2, 110, 2});
    positions.mark({1, 3, "SYNTH", 107, 109, 10, 10});
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").average_price, 105);
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").unrealized_pnl, 12);
    positions.apply_fill({3, "SYNTH", Side::Sell, 1, 115, 4});
    EXPECT_EQ(positions.get("SYNTH").quantity, 3);
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").realized_pnl, 10);
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").unrealized_pnl, 9);
    EXPECT_DOUBLE_EQ(positions.total_pnl(), 19);
}

TEST(Positions, ReversesLongToShortThenCloses) {
    PositionManager positions;
    positions.apply_fill({1, "SYNTH", Side::Buy, 2, 100, 1});
    positions.apply_fill({2, "SYNTH", Side::Sell, 5, 110, 2});
    positions.mark({1, 3, "SYNTH", 104, 106, 10, 10});
    const auto short_position = positions.get("SYNTH");
    EXPECT_EQ(short_position.quantity, -3);
    EXPECT_DOUBLE_EQ(short_position.average_price, 110);
    EXPECT_DOUBLE_EQ(short_position.realized_pnl, 20);
    EXPECT_DOUBLE_EQ(short_position.unrealized_pnl, 15);
    positions.apply_fill({3, "SYNTH", Side::Buy, 3, 105, 4});
    EXPECT_EQ(positions.get("SYNTH").quantity, 0);
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").average_price, 0);
    EXPECT_DOUBLE_EQ(positions.total_pnl(), 35);
}

TEST(Positions, ReversesShortToLongAndSeparatesSymbols) {
    PositionManager positions;
    positions.apply_fill({1, "SYNTH", Side::Sell, 2, 110, 1});
    positions.apply_fill({2, "SYNTH", Side::Buy, 3, 100, 2});
    positions.mark({1, 3, "SYNTH", 101, 103, 10, 10});
    positions.apply_fill({3, "OTHER", Side::Sell, 1, 50, 4});
    positions.mark({2, 5, "OTHER", 54, 56, 10, 10});
    EXPECT_EQ(positions.get("SYNTH").quantity, 1);
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").average_price, 100);
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").realized_pnl, 20);
    EXPECT_DOUBLE_EQ(positions.get("OTHER").unrealized_pnl, -5);
    EXPECT_DOUBLE_EQ(positions.total_pnl(), 17);
    EXPECT_EQ(positions.get("MISSING").quantity, 0);
}

TEST(Positions, RejectsInvalidOrExcessiveFillsWithoutMutation) {
    PositionManager positions;
    EXPECT_THROW(positions.apply_fill(Fill{1, "SYNTH", Side::Buy, 0, 100, 1}), std::invalid_argument);
    positions.apply_fill({1, "SYNTH", Side::Buy, max_quantity, 100, 1});
    EXPECT_THROW(positions.apply_fill(Fill{2, "SYNTH", Side::Buy, 1, 100, 2}), std::overflow_error);
    EXPECT_EQ(positions.get("SYNTH").quantity, max_quantity);
}
