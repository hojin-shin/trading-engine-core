#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace trading {

using Quantity = std::int64_t;
using OrderId = std::uint64_t;
using Timestamp = std::uint64_t;
inline constexpr Quantity max_quantity = 1'000'000'000;
inline constexpr double max_price = 1'000'000'000'000.0;

enum class Side { Buy, Sell };
enum class OrderType { Market, Limit };
enum class OrderState { PendingRisk, Accepted, PartiallyFilled, Filled, Cancelled, Rejected };

struct MarketData {
    std::uint64_t sequence{};
    Timestamp timestamp_ns{};
    std::string symbol;
    double bid{};
    double ask{};
    Quantity bid_size{};
    Quantity ask_size{};
    [[nodiscard]] double mid() const { return bid + (ask - bid) / 2.0; }
};

struct Signal {
    std::string symbol;
    Side side{Side::Buy};
    Quantity quantity{};
    OrderType type{OrderType::Market};
    std::optional<double> limit_price;
};

struct Order {
    OrderId id{};
    Signal request;
    OrderState state{OrderState::PendingRisk};
    Quantity filled_quantity{};
    double average_fill_price{};
    std::string rejection_reason;
    [[nodiscard]] Quantity remaining() const { return request.quantity - filled_quantity; }
};

struct CancelRequest {
    OrderId order_id{};
};

// Cancellation rejection does not change the target order's state.
struct CancelResult {
    OrderId order_id{};
    bool cancelled{};
    std::string reason;
};

struct Fill {
    OrderId order_id{};
    std::string symbol;
    Side side{Side::Buy};
    Quantity quantity{};
    double price{};
    Timestamp timestamp_ns{};
};

struct Position {
    std::string symbol;
    Quantity quantity{};
    double average_price{};
    double mark_price{};
    double realized_pnl{};
    double unrealized_pnl{};
};

[[nodiscard]] bool valid_price(double value);
[[nodiscard]] bool valid_side(Side side);
[[nodiscard]] bool is_working(OrderState state);
[[nodiscard]] std::string_view to_string(OrderState state);
void validate_market_data(const MarketData& data);
void validate_signal(const Signal& signal);
void validate_fill(const Fill& fill);

} // namespace trading
