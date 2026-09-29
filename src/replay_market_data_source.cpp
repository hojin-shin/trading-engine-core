#include "trading/replay_market_data_source.hpp"

#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace trading {
namespace {

template <typename T>
T parse_number(std::string_view text) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument("Invalid numeric field");
    }
    return value;
}

std::vector<std::string_view> split(const std::string& line) {
    std::vector<std::string_view> fields;
    std::string_view remaining(line);
    while (true) {
        const auto comma = remaining.find(',');
        fields.push_back(remaining.substr(0, comma));
        if (comma == std::string_view::npos) { break; }
        remaining.remove_prefix(comma + 1);
    }
    return fields;
}

void strip_cr(std::string& line) {
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
}

} // namespace

ReplayMarketDataSource::ReplayMarketDataSource(std::vector<MarketData> data)
    : data_(std::move(data)) {
    std::uint64_t sequence = 0;
    Timestamp timestamp = 0;
    for (const auto& tick : data_) {
        validate_market_data(tick);
        if (tick.sequence <= sequence || tick.timestamp_ns < timestamp) {
            throw std::invalid_argument("Replay must have increasing sequences and nondecreasing timestamps");
        }
        sequence = tick.sequence;
        timestamp = tick.timestamp_ns;
    }
}

ReplayMarketDataSource ReplayMarketDataSource::from_csv(std::istream& input) {
    std::string line;
    if (!std::getline(input, line)) { throw std::invalid_argument("Missing CSV header"); }
    strip_cr(line);
    if (line != "sequence,timestamp_ns,symbol,bid,ask,bid_size,ask_size") {
        throw std::invalid_argument("Unexpected CSV header");
    }
    std::vector<MarketData> data;
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        strip_cr(line);
        try {
            const auto fields = split(line);
            if (fields.size() != 7) { throw std::invalid_argument("Expected seven columns"); }
            MarketData tick{parse_number<std::uint64_t>(fields[0]),
                            parse_number<Timestamp>(fields[1]), std::string(fields[2]),
                            parse_number<double>(fields[3]), parse_number<double>(fields[4]),
                            parse_number<Quantity>(fields[5]), parse_number<Quantity>(fields[6])};
            validate_market_data(tick);
            if (!data.empty() && (tick.sequence <= data.back().sequence ||
                                  tick.timestamp_ns < data.back().timestamp_ns)) {
                throw std::invalid_argument("Out-of-order replay record");
            }
            data.push_back(std::move(tick));
        } catch (const std::invalid_argument& error) {
            throw std::invalid_argument("CSV line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    if (input.bad() || (input.fail() && !input.eof())) {
        throw std::runtime_error("Failed to read CSV input");
    }
    return ReplayMarketDataSource(std::move(data));
}

std::optional<MarketData> ReplayMarketDataSource::next() {
    if (cursor_ == data_.size()) { return std::nullopt; }
    return data_[cursor_++];
}

std::vector<MarketData> sample_market_data() {
    return {{1, 1'000, "SYNTH", 99.0, 101.0, 10, 10},
            {2, 2'000, "SYNTH", 102.0, 104.0, 10, 10},
            {3, 3'000, "SYNTH", 104.0, 106.0, 10, 10},
            {4, 4'000, "SYNTH", 103.0, 105.0, 10, 10}};
}

} // namespace trading
