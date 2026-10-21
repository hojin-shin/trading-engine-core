#include "trading/instrument_catalog.hpp"
#include "trading/simulated_exchange.hpp"
#include "trading/trading_engine.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
using namespace trading;

namespace {
InstrumentCatalog contracts() {
    return InstrumentCatalog(std::vector<Instrument>{{"SYNTH", 0.25, 10}, {"DIME", 0.1, 5}});
}
class Script final : public IStrategy {
public:
    explicit Script(std::map<std::uint64_t, StrategyAction> actions) : actions_(std::move(actions)) {}
    std::optional<StrategyAction> on_market_data(const MarketData& data) override {
        const auto found = actions_.find(data.sequence);
        if (found == actions_.end()) { return std::nullopt; }
        return found->second;
    }
private:
    std::map<std::uint64_t, StrategyAction> actions_;
};
class OffGridGateway final : public IExecutionGateway {
public:
    std::vector<Fill> on_market_data(const MarketData&) override { return {}; }
    std::vector<Fill> submit(const Order& order) override {
        return {{order.id, order.request.symbol, order.request.side, 1, 100.1, 1000, 1}};
    }
    bool cancel(OrderId) override { return true; }
};
}

TEST(Instruments, DefaultsAndExplicitCatalogHaveNoUnknownSymbolFallback) {
    const InstrumentCatalog defaults;
    EXPECT_DOUBLE_EQ(defaults.get("SYNTH").tick_size, 0.01);
    EXPECT_DOUBLE_EQ(defaults.get("OTHER").contract_multiplier, 1);
    EXPECT_THROW((void)defaults.get("UNKNOWN"), std::invalid_argument);
    const auto catalog = contracts();
    EXPECT_DOUBLE_EQ(catalog.get("SYNTH").contract_multiplier, 10);
    EXPECT_DOUBLE_EQ(catalog.get("DIME").tick_size, 0.1);
    EXPECT_THROW((void)catalog.get("OTHER"), std::invalid_argument);
}

TEST(Instruments, RejectsInvalidMetadataAndDuplicateSymbols) {
    for (double value : {0.0, -1.0, max_price * 2,
                         std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_THROW((InstrumentCatalog(std::vector<Instrument>{{"SYNTH", value, 1}})), std::invalid_argument);
        EXPECT_THROW((InstrumentCatalog(std::vector<Instrument>{{"SYNTH", 0.25, value}})), std::invalid_argument);
    }
    EXPECT_THROW((InstrumentCatalog(std::vector<Instrument>{{"", 0.25, 1}})), std::invalid_argument);
    EXPECT_THROW((InstrumentCatalog(std::vector<Instrument>{{"SYNTH", 0.25, 1}, {"SYNTH", 1, 10}})),
                 std::invalid_argument);
}

TEST(Instruments, TickGridSupportsDecimalRepresentationWithoutRoundingBadPrices) {
    const auto catalog = contracts();
    EXPECT_NO_THROW(catalog.validate_price("SYNTH", 100.25));
    EXPECT_NO_THROW(catalog.validate_price("DIME", 0.1 + 0.2));
    EXPECT_NO_THROW(catalog.validate_price("DIME", std::nextafter(100.1, 101.0)));
    for (double price : {100.1, 100.125, 100.249, 0.01, 0.0, -1.0,
                         std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_THROW(catalog.validate_price("SYNTH", price), std::invalid_argument);
    }
    EXPECT_NO_THROW(catalog.validate_price("SYNTH", 0.25 * 1e12));
    EXPECT_THROW(catalog.validate_price("SYNTH", 0.25 * (1e12 + 1)), std::invalid_argument);
    EXPECT_THROW(catalog.validate_price("SYNTH", 0.25 * (1e12 - 0.25)), std::invalid_argument);
}

TEST(Instruments, QuotesAndExecutionsRequireGridButMidpointsAndAveragesDoNot) {
    PositionManager positions(contracts());
    positions.mark({1, 1000, "SYNTH", 100, 100.25, 10, 10});
    positions.apply_fill({1, "SYNTH", Side::Buy, 1, 100, 1000, 1});
    positions.apply_fill({1, "SYNTH", Side::Buy, 1, 100.25, 1000, 2});
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").mark_price, 100.125);
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").average_price, 100.125);
    EXPECT_DOUBLE_EQ(positions.total_pnl(), 0);
    EXPECT_THROW(positions.mark({2, 2000, "SYNTH", 100.1, 101, 10, 10}), std::invalid_argument);
    EXPECT_THROW(positions.apply_fill(Fill{1, "SYNTH", Side::Buy, 1, 100.1, 1000, 3}), std::invalid_argument);
    EXPECT_EQ(positions.get("SYNTH").quantity, 2);
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").mark_price, 100.125);
}

TEST(Instruments, LongReductionsReversalAndCloseScalePnlExactlyOnce) {
    PositionManager positions(contracts());
    positions.mark({1, 1000, "SYNTH", 104.75, 105.25, 10, 10});
    positions.apply_fill({1, "SYNTH", Side::Buy, 2, 100, 1000, 1});
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").unrealized_pnl, 100);
    positions.apply_fill({2, "SYNTH", Side::Sell, 1, 110, 1000, 2});
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").realized_pnl, 100);
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").unrealized_pnl, 50);
    positions.apply_fill({3, "SYNTH", Side::Sell, 3, 90, 1000, 3});
    EXPECT_EQ(positions.get("SYNTH").quantity, -2);
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").average_price, 90);
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").realized_pnl, 0);
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").unrealized_pnl, -300);
    positions.apply_fill({4, "SYNTH", Side::Buy, 2, 80, 1000, 4});
    EXPECT_EQ(positions.get("SYNTH").quantity, 0);
    EXPECT_DOUBLE_EQ(positions.total_pnl(), 200);
}

TEST(Instruments, ShortReversalAndMultipleSymbolsUseTheirOwnMultipliers) {
    PositionManager positions(contracts());
    positions.mark({1, 1000, "SYNTH", 99.75, 100.25, 10, 10});
    positions.apply_fill({1, "SYNTH", Side::Sell, 2, 110, 1000, 1});
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").unrealized_pnl, 200);
    positions.apply_fill({2, "SYNTH", Side::Buy, 3, 100, 1000, 2});
    EXPECT_DOUBLE_EQ(positions.get("SYNTH").realized_pnl, 200);
    EXPECT_EQ(positions.get("SYNTH").quantity, 1);
    positions.mark({2, 2000, "DIME", 49.9, 50.1, 10, 10});
    positions.apply_fill({3, "DIME", Side::Buy, 2, 49, 2000, 3});
    EXPECT_DOUBLE_EQ(positions.get("DIME").unrealized_pnl, 10);
    EXPECT_DOUBLE_EQ(positions.total_pnl(), 210);
}

TEST(Instruments, RiskUsesMultiplierForMarketAndLimitNotionalIncludingBoundary) {
    PositionManager positions(contracts());
    const MarketData quote{1, 1000, "SYNTH", 99.75, 100, 10, 10};
    const Signal market{"SYNTH", Side::Buy, 2, OrderType::Market, {}};
    EXPECT_TRUE(RiskManager({10, 20, 2000, 1000}).check(market, quote, positions, {}).approved);
    EXPECT_EQ(RiskManager({10, 20, 1999, 1000}).check(market, quote, positions, {}).reason, "Order notional limit");
    const Signal limit{"SYNTH", Side::Buy, 2, OrderType::Limit, 100.25};
    EXPECT_TRUE(RiskManager({10, 20, 2005, 1000}).check(limit, quote, positions, {}).approved);
    EXPECT_FALSE(RiskManager({10, 20, 2004, 1000}).check(limit, quote, positions, {}).approved);
    const Signal sell{"SYNTH", Side::Sell, 2, OrderType::Market, {}};
    EXPECT_TRUE(RiskManager({10, 20, 1995, 1000}).check(sell, quote, positions, {}).approved);
    EXPECT_FALSE(RiskManager({10, 20, 1994, 1000}).check(sell, quote, positions, {}).approved);
}

TEST(Instruments, SessionLossGateUsesScaledMidpointPnl) {
    PositionManager positions(contracts());
    const MarketData quote{1, 1000, "SYNTH", 100, 100.25, 10, 10};
    positions.mark(quote);
    positions.apply_fill({1, "SYNTH", Side::Buy, 2, 100.25, 1000, 1});
    EXPECT_DOUBLE_EQ(positions.total_pnl(), -2.5);
    const Signal request{"SYNTH", Side::Sell, 1, OrderType::Market, {}};
    EXPECT_EQ(RiskManager({10, 20, 10000, 2.5}).check(request, quote, positions, {}).reason, "Session loss limit");
    EXPECT_TRUE(RiskManager({10, 20, 10000, 2.6}).check(request, quote, positions, {}).approved);
}

TEST(Instruments, OrderManagerRejectsInvalidPricesAndDoesNotConsumeExecutionId) {
    OrderManager orders(contracts());
    const auto invalid = orders.create({"SYNTH", Side::Buy, 2, OrderType::Limit, 100.1});
    EXPECT_THROW(orders.accept(invalid), std::invalid_argument);
    EXPECT_EQ(orders.get(invalid).state, OrderState::PendingRisk);
    const auto id = orders.create({"SYNTH", Side::Buy, 2, OrderType::Limit, 100.25});
    orders.accept(id);
    Fill fill{id, "SYNTH", Side::Buy, 1, 100.1, 1000, 1};
    EXPECT_THROW(orders.apply_fill(fill), std::invalid_argument);
    EXPECT_EQ(orders.get(id).filled_quantity, 0);
    fill.price = 100;
    EXPECT_TRUE(orders.apply_fill(fill));
    EXPECT_FALSE(orders.apply_fill(fill));
}

TEST(Instruments, EngineRoundTripScalesRealizedPnlAndRetainsExecutionIds) {
    ReplayMarketDataSource source(sample_market_data());
    TradingEngine engine(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>(),
                         {}, 1, nullptr, contracts());
    engine.run(source);
    ASSERT_EQ(engine.fills().size(), 2U);
    EXPECT_EQ(engine.fills()[0].execution_id, 1U);
    EXPECT_EQ(engine.fills()[1].execution_id, 2U);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 0);
    EXPECT_DOUBLE_EQ(engine.positions().total_pnl(), 60);
}

TEST(Instruments, OffGridNewLimitIsRejectedWithReasonAndNoFill) {
    ReplayMarketDataSource source(sample_market_data());
    auto strategy = std::make_unique<Script>(std::map<std::uint64_t, StrategyAction>{
        {1, Signal{"SYNTH", Side::Buy, 2, OrderType::Limit, 100.1}}});
    TradingEngine engine(std::move(strategy), std::make_unique<SimulatedExchange>(), {}, 1, nullptr, contracts());
    engine.run(source);
    EXPECT_TRUE(engine.fills().empty());
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Rejected);
    EXPECT_EQ(engine.orders().get(1).rejection_reason, "Price is off tick grid: SYNTH");
}

TEST(Instruments, OffGridReplacementLeavesOriginalWorkingUntilLaterFill) {
    ReplayMarketDataSource source({{1, 1000, "SYNTH", 99, 101, 10, 10},
                                   {2, 2000, "SYNTH", 99, 101, 10, 10},
                                   {3, 3000, "SYNTH", 99, 100, 10, 10}});
    TradingEngine engine(std::make_unique<ReplaceExampleStrategy>("SYNTH", 2, 100, 100.1),
                         std::make_unique<SimulatedExchange>(), {}, 1, nullptr, contracts());
    engine.run(source);
    ASSERT_EQ(engine.replace_results().size(), 1U);
    const auto& result = engine.replace_results()[0];
    EXPECT_FALSE(result.original_cancelled);
    EXPECT_FALSE(result.replacement_order_id);
    EXPECT_EQ(result.reason, "Price is off tick grid: SYNTH");
    EXPECT_EQ(engine.orders().orders().size(), 1U);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Filled);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").average_price, 100);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").unrealized_pnl, -10);
}

TEST(Instruments, ValidReplacementRechecksScaledNotionalAfterCancel) {
    ReplayMarketDataSource source({{1, 1000, "SYNTH", 99, 101, 10, 10},
                                   {2, 2000, "SYNTH", 99, 101, 10, 10}});
    TradingEngine engine(std::make_unique<ReplaceExampleStrategy>("SYNTH", 2, 100, 100.25),
                         std::make_unique<SimulatedExchange>(), {10, 20, 2000, 1000}, 1, nullptr, contracts());
    engine.run(source);
    ASSERT_EQ(engine.replace_results().size(), 1U);
    EXPECT_TRUE(engine.replace_results()[0].original_cancelled);
    EXPECT_FALSE(engine.replace_results()[0].replaced);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(2).state, OrderState::Rejected);
    EXPECT_EQ(engine.orders().get(2).rejection_reason, "Order notional limit");
}

TEST(Instruments, UnknownOrOffGridQuoteStopsWithoutCreatingPositionOrOrders) {
    for (bool unknown : {false, true}) {
        ReplayMarketDataSource source({{1, 1000, unknown ? "UNKNOWN" : "SYNTH", 99.1, 101, 10, 10}});
        TradingEngine engine(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>(),
                             {}, 1, nullptr, contracts());
        EXPECT_THROW(engine.run(source), std::invalid_argument);
        EXPECT_TRUE(engine.orders().orders().empty());
        EXPECT_TRUE(engine.positions().positions().empty());
        EXPECT_TRUE(engine.fills().empty());
    }
}

TEST(Instruments, OffGridGatewayFillStopsBeforeOrderOrPnlMutation) {
    ReplayMarketDataSource source(sample_market_data());
    TradingEngine engine(std::make_unique<ExampleStrategy>(), std::make_unique<OffGridGateway>(),
                         {}, 1, nullptr, contracts());
    EXPECT_THROW(engine.run(source), std::invalid_argument);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(1).filled_quantity, 0);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 0);
    EXPECT_DOUBLE_EQ(engine.positions().total_pnl(), 0);
    EXPECT_TRUE(engine.fills().empty());
}
