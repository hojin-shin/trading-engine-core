#include "trading/trading_engine.hpp"
#include "trading/simulated_exchange.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <stdexcept>
using namespace trading;
namespace {
constexpr auto header = "sequence,timestamp_ns,symbol,bid,ask,bid_size,ask_size\n";
constexpr auto row = "1,1000,SYNTH,99,101,10,1\n";
class NonSeekBuffer final : public std::stringbuf {
public:
    using std::stringbuf::stringbuf;
protected:
    pos_type seekoff(off_type, std::ios_base::seekdir, std::ios_base::openmode) override {
        throw std::runtime_error("Unexpected seek");
    }
    pos_type seekpos(pos_type, std::ios_base::openmode) override {
        throw std::runtime_error("Unexpected seek");
    }
};
}

TEST(StreamingReplay, ReadsOnlyHeaderThenOneRowAtATime) {
    const std::string text = std::string(header) + row + "bad\n";
    std::istringstream input(text);
    CsvReplayMarketDataSource source(input);
    EXPECT_EQ(input.tellg(), static_cast<std::streamoff>(std::string(header).size()));
    ASSERT_TRUE(source.next());
    EXPECT_EQ(input.tellg(), static_cast<std::streamoff>(std::string(header).size() + std::string(row).size()));
    try {
        (void)source.next();
        FAIL() << "Expected malformed third line";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("CSV line 3"), std::string::npos);
    }
    input.clear();
    EXPECT_THROW(source.next(), std::logic_error);
}

TEST(StreamingReplay, SupportsNonSeekableCrLfEqualTimestampsAndUnterminatedLastRow) {
    NonSeekBuffer buffer("sequence,timestamp_ns,symbol,bid,ask,bid_size,ask_size\r\n"
                         "1,1000,SYNTH,99,101,10,1\r\n2,1000,OTHER,1,2,0,0");
    std::istream input(&buffer);
    CsvReplayMarketDataSource source(input);
    EXPECT_EQ(source.next()->sequence, 1U);
    const auto second = source.next();
    ASSERT_TRUE(second);
    EXPECT_EQ(second->symbol, "OTHER");
    EXPECT_FALSE(source.next());
    EXPECT_FALSE(source.next());
}

TEST(StreamingReplay, HeaderOnlyAndMissingOrWrongHeaders) {
    std::istringstream input(header);
    CsvReplayMarketDataSource source(input);
    EXPECT_FALSE(source.next());
    EXPECT_FALSE(source.next());
    for (const auto* text : {"", "wrong\n"}) {
        std::istringstream invalid(text);
        EXPECT_THROW(CsvReplayMarketDataSource bad(invalid), std::invalid_argument);
    }
}

TEST(StreamingReplay, StrictValidationRejectsMalformedAndOutOfOrderRows) {
    for (const auto* next : {"", "2,1000,SYNTH,99x,101,1,1", "2,1000,SYNTH,nan,101,1,1",
                            "2,1000,SYNTH,102,101,1,1", "2,1000,SYNTH,99,101,-1,1",
                            "2,1000,,99,101,1,1", "2,1000,SYNTH,99,101,1",
                            "2,1000,SYNTH,99,101,1,1,extra", "-1,1000,SYNTH,99,101,1,1",
                            "1,1000,SYNTH,99,101,1,1", "2,999,SYNTH,99,101,1,1"}) {
        SCOPED_TRACE(next);
        std::istringstream input(std::string(header) + row + next + "\n");
        CsvReplayMarketDataSource source(input);
        ASSERT_TRUE(source.next());
        EXPECT_THROW(source.next(), std::invalid_argument);
        EXPECT_THROW(source.next(), std::logic_error);
    }
}

TEST(StreamingReplay, IoFailureIsTerminalAndDistinctFromEof) {
    std::istringstream input(std::string(header) + row);
    CsvReplayMarketDataSource source(input);
    input.setstate(std::ios::badbit);
    EXPECT_THROW(source.next(), std::runtime_error);
    input.clear();
    EXPECT_THROW(source.next(), std::logic_error);
    std::istringstream broken(header);
    broken.setstate(std::ios::badbit);
    EXPECT_THROW(CsvReplayMarketDataSource bad(broken), std::runtime_error);
}

TEST(StreamingReplay, ExceptionEnabledStreamsHandleNormalEofAndIoFailure) {
    for (bool newline : {false, true}) {
        std::istringstream input(std::string(header) + "1,1000,SYNTH,99,101,10,1" + (newline ? "\n" : ""));
        input.exceptions(std::ios::badbit | std::ios::failbit | std::ios::eofbit);
        CsvReplayMarketDataSource source(input);
        ASSERT_TRUE(source.next());
        EXPECT_FALSE(source.next());
        EXPECT_FALSE(source.next());
    }
    std::istringstream input(header);
    CsvReplayMarketDataSource source(input);
    input.exceptions(std::ios::badbit);
    EXPECT_THROW(input.setstate(std::ios::badbit), std::ios_base::failure);
    EXPECT_THROW(source.next(), std::runtime_error);
}

TEST(StreamingReplay, MatchesBufferedReplayAndPreservesPartialFillAccounting) {
    const auto text = std::string(header) + row + "2,2000,SYNTH,100,102,10,1\n"
                                             "3,3000,SYNTH,104,106,10,10\n";
    std::istringstream streaming_input(text), buffered_input(text);
    CsvReplayMarketDataSource streaming(streaming_input);
    auto buffered = ReplayMarketDataSource::from_csv(buffered_input);
    std::ostringstream streaming_trace, buffered_trace;
    TradingEngine first(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>(), {}, 1, &streaming_trace);
    TradingEngine second(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>(), {}, 1, &buffered_trace);
    first.run(streaming);
    second.run(buffered);
    EXPECT_EQ(first.processed_ticks(), 3U);
    EXPECT_EQ(first.fills(), second.fills());
    EXPECT_EQ(streaming_trace.str(), buffered_trace.str());
    EXPECT_DOUBLE_EQ(first.positions().total_pnl(), 5);
}

TEST(StreamingReplay, MalformedSuffixDrainsValidTicksAndCancelsWorkingRemainder) {
    std::istringstream input(std::string(header) + row + "bad\n");
    CsvReplayMarketDataSource source(input);
    std::ostringstream trace;
    TradingEngine engine(std::make_unique<ExampleStrategy>(), std::make_unique<SimulatedExchange>(), {}, 1, &trace);
    EXPECT_THROW(engine.run(source), std::invalid_argument);
    EXPECT_EQ(engine.processed_ticks(), 1U);
    ASSERT_EQ(engine.fills().size(), 1U);
    EXPECT_EQ(engine.orders().get(1).state, OrderState::Cancelled);
    EXPECT_EQ(engine.orders().get(1).filled_quantity, 1);
    EXPECT_EQ(engine.positions().get("SYNTH").quantity, 1);
    EXPECT_DOUBLE_EQ(engine.positions().get("SYNTH").unrealized_pnl, -1);
    EXPECT_NE(trace.str().find("reason=run_error"), std::string::npos);
}
