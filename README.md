# trading-engine-core

A clean-room C++20 portfolio implementation of a small event-driven trading system.
The first vertical slice runs from deterministic market data through strategy, risk,
orders, simulated execution, and position/PnL accounting. It uses only synthetic data
and a scripted example strategy; it makes no claims about trading performance.

Proprietary strategies, employer-specific code, broker APIs, credentials, DMA
protocol details, and production-sensitive components are intentionally excluded.

## Architecture

```mermaid
flowchart LR
    Replay[CSV / sample replay thread] --> Queue[Bounded thread-safe queue]
    Queue --> Engine[TradingEngine consumer thread]
    Engine --> Strategy[IStrategy / ExampleStrategy]
    Strategy --> Risk[RiskManager]
    Risk --> Orders[OrderManager]
    Orders --> Gateway[IExecutionGateway / SimulatedExchange]
    Gateway --> Fills[Fills]
    Fills --> Orders
    Fills --> Positions[PositionManager / PnL]
    Positions --> Risk
    Orders --> Risk
```

Two execution threads: a replay producer and the calling engine thread. Only the
bounded queue crosses threads; mutable trading state belongs to the consumer.
For each tick, the engine marks positions, fills existing orders, calls the strategy,
checks risk, and submits any new order using the remaining quote liquidity, or
processes a strategy cancellation or price replacement request. Matching precedes the strategy action.
Sequence numbers are globally increasing; timestamps are nondecreasing nanoseconds.
No wall-clock sleeps are used. Output is deterministic.

## Build and run on Ubuntu

Requires CMake 3.24+, GCC 12+ or Clang 16+, and a C++20 standard library.

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/trading_demo
./build/trading_demo data/sample.csv
```

GoogleTest 1.14+ is used if installed with a CMake package config. Otherwise CMake
downloads GoogleTest 1.15.2 with a pinned SHA-256 hash. Initial configuration needs
network access. For offline builds, provide a previously downloaded source tree:

```sh
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest
```

Use `-DBUILD_TESTING=OFF` for a dependency-free library/demo build. Optional Linux
AddressSanitizer + UndefinedBehaviorSanitizer checks:

```sh
cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTRADING_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel 2
ctest --test-dir build-sanitize --output-on-failure
```

The Ubuntu GitHub Actions workflow builds with GCC in Release and Clang in Debug
with sanitizers, then runs GoogleTest, built-in replay, and CSV replay checks.

## Expected demo output

```text
ticks=4 orders=2 fills=2
order=1 state=Filled filled=2 average_price=101.00
order=2 state=Filled filled=2 average_price=104.00
SYNTH position=0 realized_pnl=6.00 unrealized_pnl=0.00
```

`ExampleStrategy` buys two units on the first `SYNTH` tick and sells two on the third.
It is a fixed demonstration script, not a position-aware trading algorithm. Its
second order is emitted even if the first was rejected or only partially filled.

## Trace replay events

Add `--trace` to see the intermediate steps before the final summary:

```sh
./build/trading_demo --trace
./build/trading_demo --trace data/sample.csv
```

Trace lines include the current tick sequence and timestamp, then `event=TICK`,
`ORDER`, `FILL`, or `POSITION`. Order snapshots show each state transition, filled
and unfilled quantities, average fill price, and rejection/cancellation reasons.
Position snapshots are emitted after midpoint marking and after each fill.
`unfilled` is requested minus filled quantity; on a cancelled order it is the
cancelled quantity, not an active working quantity. EOF cancellation uses
`reason=end_of_replay`; cleanup following an exception uses `reason=run_error`.
Cancellation does not undo fills or close positions.

For a partial fill followed by cancellation, create a separate CSV with only:

```csv
sequence,timestamp_ns,symbol,bid,ask,bid_size,ask_size
1,1000,SYNTH,99,101,10,1
```

Run it with `./build/trading_demo --trace /path/to/sample-cancel.csv`. Order 1
transitions through `PendingRisk -> Accepted -> PartiallyFilled -> Cancelled`;
one unit remains long at 101 with midpoint 100 and unrealized PnL -1.
Keep `data/sample.csv` unchanged: the CSV smoke test expects its original PnL +6.

Library tracing is opt-in via the final `TradingEngine` constructor parameter,
an `std::ostream*` that must remain valid through `run()`. Output occurs only on
the consumer thread; callers must not write to the same stream concurrently.
Formatting preserves the destination stream settings. Tracing is synchronous
and can slow replay. Stream failures disable tracing without interrupting order
processing or cleanup; this diagnostic stream is not a durable audit log.

## Limit order replay

Use a fixed, synthetic limit-order script with the dedicated CSV:

```sh
./build/trading_demo --trace --limit-demo data/sample_limit.csv
```

`--limit-demo` requires an explicit CSV. It changes the example strategy to submit
a buy of two units at limit 100 on the first matching tick and a sell of two units
at limit 104 on the third. Both orders can rest until a later executable quote:

| Tick | Bid / ask | Result |
| --- | --- | --- |
| 1 | 99 / 101 | Buy limit 100 accepted; ask is too high, so no fill |
| 2 | 98 / 100 | Resting buy fills two units at ask 100 |
| 3 | 103 / 105 | Sell limit 104 accepted; bid is too low, so no fill |
| 4 | 104 / 106 | Resting sell fills two units at bid 104 |

Final output after the trace:

```text
ticks=4 orders=2 fills=2
order=1 state=Filled filled=2 average_price=100.00
order=2 state=Filled filled=2 average_price=104.00
SYNTH position=0 realized_pnl=8.00 unrealized_pnl=0.00
```

Buys require `ask <= limit`; sells require `bid >= limit`, with available liquidity.
Execution uses the current quote, so a better price is possible. Unfilled orders
remain `Accepted`, or `PartiallyFilled` after a partial fill, until another quote
allows execution or EOF cancels their remainder.

This is still a scripted example: the third-tick sell is emitted even if the buy
was not filled, and can open a short position with a different CSV. It is not a
position-aware exit rule. Default runs without `--limit-demo` retain market orders.

## Cancel during replay

Submit a buy limit of two units at 100 on matching tick 1, then request its
cancellation on matching tick 2. This script sends no sell order:

```sh
./build/trading_demo --trace --cancel-demo data/sample_cancel.csv
./build/trading_demo --trace --cancel-demo data/sample_cancel_partial.csv
```

The first CSV keeps ask at 101 through tick 2; the order is cancelled before ask
falls to 100 on tick 3. Final state: `Cancelled`, zero fills, zero position/PnL.
The second CSV supplies only one unit at ask 100 on tick 1, then ask 101 on tick 2.
Only the remaining unit is cancelled. Tick 3 cannot fill it: position remains +1
at average 100, realized PnL 0, and unrealized PnL -1 at midpoint 99.

Look for `event=CANCEL_REQUEST`, `state=Cancelled ... reason=strategy_request`,
and `event=CANCEL_RESULT ... result=Cancelled`. A failed cancellation returns
`result=Rejected` without changing the order state. Unknown IDs, terminal orders,
and gateway declines are reported separately. A later valid request can still run.

Existing orders match each tick **before** the strategy can request cancellation.
If that tick fills the whole order, cancellation is rejected; if it fills only
part, the remaining quantity can be cancelled. A successful cancellation releases
working-order risk reservations but leaves filled positions intact. Cancellation
does not pass through new-order risk checks, so a loss-limit breach does not block it.
The gateway must acknowledge cancellation before OrderManager marks it cancelled.
Gateway exceptions stop the run and trigger existing error cleanup.

### Strategy interface

`IStrategy::on_market_data` now returns `std::optional<StrategyAction>`, where
`StrategyAction` is `std::variant<Signal, CancelRequest, ReplaceRequest>`. Return at most
one new order, cancellation, or replacement per tick, or `std::nullopt`. Existing custom strategies need
to update their return type; their `return Signal{...}` statements still work.
The optional `on_order_created(const Order&)` notification provides the actual
engine-assigned ID after initial risk/submission/fill processing, including risk
rejections. Copy fields needed for later cancellation; it is not a live view or
a notification for every later fill. The demo uses this ID, not a hard-coded ID.
Like other strategy callbacks, notification exceptions stop the run and trigger
cleanup. `cancel_results()` is available after `run()` for strategy-request results;
automatic EOF/error cleanup continues to use order logs and is not counted there.

Each demo mode requires a CSV and cannot be combined with another demo mode.
This remains synchronous simulation without exchange latency.

## Replace a limit price during replay

```sh
./build/trading_demo --trace --replace-demo data/sample_replace.csv
./build/trading_demo --trace --replace-demo data/sample_replace_partial.csv
```

`ReplaceExampleStrategy` submits a buy limit of two at 100 on matching tick 1 and
requests a new limit of 101 on tick 2 using the actual original order ID.
The first CSV leaves the original unfilled: order 1 becomes `Cancelled`, then
order 2 fills two at 101. Final position is +2, average 101, realized PnL 0,
unrealized PnL -2 at midpoint 100.
The second CSV fills one at 100 before replacement. Only the remaining unit is
submitted as order 2 and fills at 101: total position +2, average 100.50,
realized PnL 0, unrealized PnL -1. Tick 3 produces no extra fills in either case.

Look for `REPLACE_REQUEST`, old `ORDER ... state=Cancelled reason=replace_request`,
new `ORDER ... state=PendingRisk`, and `REPLACE_RESULT`. The result links
`order` and `replacement_order`, reports `original_cancelled`, and distinguishes
`Replaced` (new order admitted, possibly immediately filled) from `Rejected`.
The final summary uses the same fields; `replace_results()` exposes these results
after the run. Internal replacement cancellations do not enter `cancel_results()`.

This is **cancel followed by a new order**, not an atomic exchange amendment:

- Only price changes for working limit orders are supported. Symbol and side stay
  unchanged, and quantity is the remaining amount after this tick's matching.
- Unknown/terminal orders, invalid prices, non-limit orders, and requests on a tick
  for another symbol are rejected before cancellation. A gateway decline leaves
  the original working and creates no replacement order.
- After cancellation is acknowledged, the old reservation is released and the new
  order passes all normal risk checks. A risk rejection leaves the old order
  cancelled and records a new `Rejected` order; the original is never restored.
- Same-tick fills happen first. Full fills prevent replacement; partial fills
  reduce the replacement quantity. Quote liquidity is shared, never reset for
  the new order. A new ID gives the replacement a new matching priority.
- `on_order_created` also reports the replacement, including risk rejections.
  Gateway/callback exceptions stop the run and trigger cleanup; no completed
  replacement result is fabricated when an exception interrupts processing.

No quantity amendments, asynchronous cancel acknowledgments, or exchange-native
amend protocol are modeled.

## Behavior and scope

- **Orders:** `PendingRisk -> Rejected`, or `PendingRisk -> Accepted -> PartiallyFilled
  -> Filled`. Working orders may become `Cancelled`; immediate full fills skip
  `PartiallyFilled`. Terminal states reject subsequent transitions and fills.
- **Risk:** validates requests and quotes, order quantity, order notional, per-symbol
  worst-case position including remaining working orders, and session realized plus
  unrealized loss. Buy and sell reservations are checked independently. Limit
  notional uses the order limit; market notional uses the current executable quote.
  These are admission checks, not ongoing guarantees after prices change. The loss
  gate rejects all new orders, including reductions, and does not cancel existing
  orders or implement liquidation/daily reset.
- **Execution:** synchronous in-process gateway; buys cross the ask and sells cross
  the bid. Limits execute only at their limit or better. Each quote supplies fresh
  per-side liquidity shared across orders. Partial fills carry to later ticks.
  Market orders also carry residual quantity; they are not IOC. Matching uses
  ascending order ID, not exchange price/time priority. No queue-position model,
  latency, fees, or slippage beyond the spread is simulated.
- **Accounting:** signed integer units, weighted average cost, realized PnL on
  reductions/closures, and correct cost reset on reversals. Unrealized PnL uses the
  latest midpoint. Per-symbol contract multipliers scale realized/unrealized PnL
  and order notional; quantities remain contract counts. Prices use `double`;
  no tick rounding, currency conversion, settlement, or derivatives margin is modeled.
  Supported quantities are bounded to 1e9 units and positive prices to 1e12.
- **Shutdown:** EOF drains queued ticks, joins the producer, then cancels remaining
  orders; positions stay open and marked. Producer/consumer exceptions are surfaced
  after joining and attempting cancellation. The engine is single-use. Accessors
  are only safe after `run()` returns/throws. Sources must return promptly; a blocked
  external I/O source needs its own cancellation design.
- **Integration boundary:** the gateway supplies incremental fills with nonzero
  execution IDs unique across one gateway/engine run. OrderManager ignores exact
  retransmissions before checking order state, including after fill or cancellation.
  Reusing an ID with any changed field fails the run. Only new validated fills reach
  position/PnL accounting and the fill history. There is no reconnect recovery,
  asynchronous acknowledgment, or production reconciliation.
  Managers are single-threaded and PositionManager must receive validated fills
  through OrderManager. Invalid integration reports fail the run; there is no
  transactional rollback of already-applied reports.
- **CSV:** exact header in `data/sample.csv`, seven unquoted fields, LF or CRLF,
  finite positive non-crossed quotes and nonnegative sizes. Blank rows, numeric
  suffixes, malformed headers, and out-of-order records fail fast with diagnostics.
  CLI CSV replay validates the header up front and rows incrementally on the producer
  thread. A later malformed row stops input; already queued valid ticks drain before
  run_error cleanup cancels working orders. Prior fills/PnL remain applied.

## Repository layout

```text
app/                     Command-line replay demo
include/trading/         Domain types, interfaces, queue, component headers
src/                     Engine and component implementations
tests/                   GoogleTest unit and integration tests
data/sample.csv          Synthetic deterministic input
.github/workflows/ci.yml Ubuntu GCC/Clang build and test jobs
```

Tests cover queue wakeup/drain/backpressure, risk reservations, order transitions,
partial fills and limit prices, long/short reversals and PnL, strict CSV parsing,
end-to-end replay, and failure/shutdown paths.

## Execution IDs and duplicate reports

`Fill::execution_id` is a required nonzero `ExecutionId` (`uint64_t`). The simulated
exchange assigns deterministic increasing IDs across orders and partial fills;
replacement orders do not reset the counter. Custom gateways and fill fixtures must
provide IDs explicitly. An identical report with a new ID is a separate execution.

`OrderManager::apply_fill` returns true for a new validated execution and false for
an exact retransmission. Callers must forward only true results to PositionManager,
which does not deduplicate independently. Identity comparison includes order, symbol,
side, incremental quantity, price, and execution timestamp. Prices compare exactly,
without tolerance. Invalid or conflicting reports throw without changing that order
or recording that report's ID. Previously accepted reports remain applied.

Trace `FILL` lines include `execution_id`; ignored reports emit `FILL_DUPLICATE` and
do not emit another fill/position update or increase the final fill count.
The in-memory ID history lasts for one engine run and grows with unique executions.
Persistence, restart recovery, multiple venues, trade corrections/busts, and delayed
new fills after synchronous cancellation are outside this increment's scope.

## Streaming CSV input

The CLI uses `CsvReplayMarketDataSource`: construction reads only the header, and
each `next()` reads and validates one row. It keeps no tick history and supports
non-seekable streams, CRLF, equal timestamps, and a final row without a newline.
Input memory scales with the current row and bounded replay queue rather than file
length. This is not a bound on total engine memory: orders, fills, and execution ID
history still grow, and individual row length is not capped.

The source borrows an `std::istream`; it must stay alive until `run()` returns or
throws. Do not access it concurrently. EOF remains EOF; parse/read failures make the
source terminal, so subsequent calls throw instead of skipping invalid rows. Header
errors occur during construction; row errors include their physical CSV line number.
I/O failures throw runtime_error, including streams with exception masks enabled.

For callers needing up-front validation, `ReplayMarketDataSource::from_csv` remains
available and intentionally buffers the entire file using the same parser. The
vector-based source and default synthetic replay are unchanged. Streaming validates
as it runs, so a malformed suffix can be discovered after valid rows caused trades;
this is a simulator, and neither input failure nor cleanup rolls those trades back.

## Instrument metadata

`InstrumentCatalog` holds immutable per-symbol `tick_size` and `contract_multiplier`.
The default contains only synthetic SYNTH and OTHER, both tick 0.01 and multiplier 1.
Unknown symbols, duplicate definitions, empty symbols, and nonpositive/nonfinite
metadata fail explicitly. Metadata values are bounded to 1e12. Custom library callers
pass a catalog as the final TradingEngine argument; managers own immutable copies.
RiskManager uses the PositionManager catalog as its source of contract metadata.
Standalone managers must be configured consistently with one another.

Limit prices, quote bid/ask, and execution prices must lie on the symbol's tick grid.
No rounding is performed. Invalid new orders are risk-rejected; invalid replacement
prices are rejected before cancelling the original. Invalid quotes or execution
reports stop the run and trigger existing cleanup. The simulator matches the engine's
validated orders/quotes; it has no independent catalog. CSV readers retain generic
format validation; instrument validation occurs at the engine/manager boundary.
Midpoints and weighted average costs may fall between ticks and are not rounded.

Tick checks allow floating-point representation error (8 machine epsilons times the
tick count), with at most 1e12 ticks per price. This is a constant positive-price grid,
not variable tick ladders or an integer/fixed-point price implementation.

PnL = price difference * contracts * contract_multiplier. Order notional = limit
price (or executable quote for market orders) * contracts * contract_multiplier.
This is a synthetic linear-contract convention, not options delta exposure or margin.
All symbols share one accounting unit; no currency conversion or fees are included.

Run the multiplier example using the original synthetic CSV:

```sh
./build/trading_demo --trace --contract-demo data/sample.csv
```

This selects SYNTH tick 0.25 and multiplier 10, prints its metadata, buys two at 101,
and sells two at 104: final realized PnL is 60.00. Without --contract-demo the same CSV
still produces 6.00. The flag may also be combined with one existing order demo mode.
Only synthetic specifications are used; no real exchange/broker data is included.

Next increment: replay throughput measurements. See WORK_STATUS.md for the current
handoff and verification instructions.
