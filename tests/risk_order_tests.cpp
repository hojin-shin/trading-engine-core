#include "trading/risk_manager.hpp"

#include <gtest/gtest.h>

#include <limits>

using namespace trading;

namespace {
const MarketData quote{1, 1000, "SYNTH", 99, 101, 20, 20};

Signal request(Side side = Side::Buy, Quantity quantity = 2) {
    return {"SYNTH", side, quantity, OrderType::Market, std::nullopt};
}
} // namespace

TEST(Risk, AllowsValidRequestAndExactLimits) {
    RiskManager risk({2, 2, 202, 10});
    EXPECT_TRUE(risk.check(request(), quote, {}, {}).approved);
}

TEST(Risk, RejectsQuantityNotionalAndMismatchedSymbol) {
    EXPECT_FALSE(RiskManager({1, 20, 10000, 100}).check(request(), quote, {}, {}).approved);
    EXPECT_FALSE(RiskManager({10, 20, 201, 100}).check(request(), quote, {}, {}).approved);
    auto signal = request();
    signal.symbol = "OTHER";
    EXPECT_FALSE(RiskManager().check(signal, quote, {}, {}).approved);
}

TEST(Risk, RejectsMalformedInputs) {
    auto signal = request();
    for (const auto quantity : {Quantity{0}, Quantity{-1}, max_quantity + 1}) {
        signal.quantity = quantity;
        EXPECT_FALSE(RiskManager().check(signal, quote, {}, {}).approved);
    }
    signal = request();
    signal.type = OrderType::Limit;
    EXPECT_FALSE(RiskManager().check(signal, quote, {}, {}).approved);
    signal.limit_price = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(RiskManager().check(signal, quote, {}, {}).approved);
    signal.limit_price = 100;
    EXPECT_TRUE(RiskManager().check(signal, quote, {}, {}).approved);
    auto bad_quote = quote;
    bad_quote.bid = 102;
    EXPECT_FALSE(RiskManager().check(request(), bad_quote, {}, {}).approved);
    signal = request();
    signal.side = static_cast<Side>(99);
    EXPECT_FALSE(RiskManager().check(signal, quote, {}, {}).approved);
}

TEST(Risk, ReservesRemainingOrdersWithoutNettingOpposingSides) {
    RiskManager risk({10, 5, 10000, 100});
    OrderManager orders;
    const auto buy = orders.create(request(Side::Buy, 4));
    orders.accept(buy);
    const auto sell = orders.create(request(Side::Sell, 4));
    orders.accept(sell);
    EXPECT_FALSE(risk.check(request(Side::Buy, 2), quote, {}, orders).approved);
    EXPECT_FALSE(risk.check(request(Side::Sell, 2), quote, {}, orders).approved);
    EXPECT_TRUE(risk.check(request(Side::Buy, 1), quote, {}, orders).approved);
    orders.cancel(buy);
    EXPECT_TRUE(risk.check(request(Side::Buy, 5), quote, {}, orders).approved);
}

TEST(Risk, CountsFilledPositionAndOnlyUnfilledRemainder) {
    RiskManager risk({10, 5, 10000, 100});
    PositionManager positions;
    OrderManager orders;
    const auto id = orders.create(request(Side::Buy, 4));
    orders.accept(id);
    const Fill fill{id, "SYNTH", Side::Buy, 2, 100, 1000};
    orders.apply_fill(fill);
    positions.apply_fill(fill);
    EXPECT_TRUE(risk.check(request(Side::Buy, 1), quote, positions, orders).approved);
    EXPECT_FALSE(risk.check(request(Side::Buy, 2), quote, positions, orders).approved);
    EXPECT_TRUE(risk.check(request(Side::Sell, 7), quote, positions, orders).approved);
    EXPECT_FALSE(risk.check(request(Side::Sell, 8), quote, positions, orders).approved);
}

TEST(Risk, UsesLimitPriceForLimitNotional) {
    Signal signal{"SYNTH", Side::Buy, 2, OrderType::Limit, 200};
    EXPECT_FALSE(RiskManager({10, 10, 300, 100}).check(signal, quote, {}, {}).approved);
}

TEST(Risk, StopsAtSessionLossIncludingUnrealizedPnl) {
    PositionManager positions;
    positions.apply_fill({1, "SYNTH", Side::Buy, 2, 110, 1000});
    positions.mark(quote);
    EXPECT_DOUBLE_EQ(positions.total_pnl(), -20);
    EXPECT_FALSE(RiskManager({10, 20, 10000, 20}).check(request(), quote, positions, {}).approved);
    EXPECT_TRUE(RiskManager({10, 20, 10000, 21}).check(request(), quote, positions, {}).approved);
}

TEST(Risk, RejectsInvalidConfiguration) {
    EXPECT_THROW(RiskManager(RiskLimits{0, 10, 100, 100}), std::invalid_argument);
    EXPECT_THROW(RiskManager(RiskLimits{10, -1, 100, 100}), std::invalid_argument);
    EXPECT_THROW(RiskManager(RiskLimits{10, 10, std::numeric_limits<double>::infinity(), 100}),
                 std::invalid_argument);
}

TEST(Orders, TracksPartialThenFullFillAndAveragePrice) {
    OrderManager orders;
    const auto id = orders.create(request(Side::Buy, 3));
    EXPECT_EQ(orders.get(id).state, OrderState::PendingRisk);
    orders.accept(id);
    orders.apply_fill({id, "SYNTH", Side::Buy, 1, 100, 1000});
    EXPECT_EQ(orders.get(id).state, OrderState::PartiallyFilled);
    EXPECT_EQ(orders.get(id).remaining(), 2);
    orders.apply_fill({id, "SYNTH", Side::Buy, 2, 103, 2000});
    EXPECT_EQ(orders.get(id).state, OrderState::Filled);
    EXPECT_DOUBLE_EQ(orders.get(id).average_fill_price, 102);
    EXPECT_THROW(orders.cancel(id), std::logic_error);
    EXPECT_THROW(orders.apply_fill(Fill{id, "SYNTH", Side::Buy, 1, 100, 3000}), std::logic_error);
}

TEST(Orders, RejectsInvalidFillsWithoutMutatingOrder) {
    OrderManager orders;
    const auto id = orders.create(request());
    EXPECT_THROW(orders.apply_fill(Fill{id, "SYNTH", Side::Buy, 1, 100, 1}), std::logic_error);
    orders.accept(id);
    EXPECT_THROW(orders.apply_fill(Fill{id, "SYNTH", Side::Buy, 3, 100, 1}), std::logic_error);
    EXPECT_THROW(orders.apply_fill(Fill{id, "OTHER", Side::Buy, 1, 100, 1}), std::logic_error);
    EXPECT_THROW(orders.apply_fill(Fill{id, "SYNTH", Side::Sell, 1, 100, 1}), std::logic_error);
    EXPECT_THROW(orders.apply_fill(Fill{id, "SYNTH", Side::Buy, 0, 100, 1}), std::invalid_argument);
    EXPECT_EQ(orders.get(id).filled_quantity, 0);
    EXPECT_EQ(orders.get(id).state, OrderState::Accepted);
}

TEST(Orders, EnforcesLimitAndTerminalTransitions) {
    OrderManager orders;
    const auto id = orders.create({"SYNTH", Side::Buy, 2, OrderType::Limit, 100});
    orders.accept(id);
    EXPECT_THROW(orders.apply_fill(Fill{id, "SYNTH", Side::Buy, 1, 101, 1}), std::logic_error);
    orders.apply_fill({id, "SYNTH", Side::Buy, 1, 99, 1});
    orders.cancel(id);
    EXPECT_EQ(orders.get(id).filled_quantity, 1);
    EXPECT_THROW(orders.accept(id), std::logic_error);
    EXPECT_THROW(orders.reject(id, "late"), std::logic_error);
    const auto rejected = orders.create(request());
    orders.reject(rejected, "Risk limit");
    EXPECT_EQ(orders.get(rejected).rejection_reason, "Risk limit");
    EXPECT_THROW(orders.accept(rejected), std::logic_error);
    EXPECT_THROW((void)orders.get(999), std::out_of_range);
}
