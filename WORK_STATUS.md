# Work status and session handoff

Updated: 2026-10-21 (Asia/Seoul).

## Read first in a new session

Read this file, README.md, VERIFICATION.md, and applicable repository instructions.
Inspect git status, git log, the actual source, and the configured build source path.
Git is authoritative for current branch, commit, pending edits, and push state.
Do not infer the current state solely from this document or an earlier chat.

## Purpose and boundaries

This is a clean-room C++20/Linux trading systems portfolio, using synthetic data
and scripted example strategies only. Do not import private systems, employer code,
broker APIs, credentials, real strategies, production data, or DMA specifications.
Keep the scope focused on trading systems engineering, not ultra-low-latency HFT.

## Repository and build

- Windows checkout: C:/tmp/signaltrue_home/dev/Globalquant_trading/trading-engine-core
- GitHub: https://github.com/hojin-shin/trading-engine-core
- Branch: main
- WSL distribution: Ubuntu-24.04
- Ubuntu source: /mnt/c/tmp/signaltrue_home/dev/Globalquant_trading/trading-engine-core
- Ubuntu build: ~/build/trading-engine-core
- Windows Git is used for the existing GitHub credentials.

```sh
cd /mnt/c/tmp/signaltrue_home/dev/Globalquant_trading/trading-engine-core
cmake -S . -B ~/build/trading-engine-core
cmake --build ~/build/trading-engine-core --parallel 2
ctest --test-dir ~/build/trading-engine-core --output-on-failure
```

## Completed increments

- Deterministic replay -> strategy -> risk -> order -> simulated execution -> PnL.
- Market/limit orders, partial fills, cancellation, and cancel/new price replacement.
- Execution IDs and exact retransmission deduplication (dfe1534).
- Streaming CSV replay with strict validation and failure cleanup (ebfcdd9).
- Instrument catalog, constant tick grids, contract multipliers, and contract demo.
  The implementation commit is identifiable by its feature message in git log;
  check Git for whether it has been pushed.

## Instrument metadata behavior

- InstrumentCatalog is immutable during processing; unknown symbols are errors.
- Defaults: synthetic SYNTH and OTHER, both tick 0.01, multiplier 1.
- Engine receives an optional final catalog argument and configures both managers
  consistently. RiskManager uses PositionManager's catalog. Standalone components
  must use consistent catalogs. SimulatedExchange relies on validated inputs.
- Validate quote bid/ask, new limit prices, and fills against the tick grid.
- Reject invalid replacements before gateway cancellation; do not remove a valid
  working order because its proposed replacement price is off grid.
- Scale realized PnL, unrealized PnL, and order notional by contract multiplier.
  Position limits and quantities remain contract counts.
- Midpoints and average costs can be off grid. No rounding is performed.
- All symbols share an accounting unit; no fees, FX, settlement, Greeks, margin,
  negative prices, variable tick ladders, or real exchange specifications.
- double prices use a small representation tolerance; max 1e12 ticks per price.
- Deduplication remains in-memory per run; restart recovery is not implemented.

## Manual checks

```sh
~/build/trading-engine-core/trading_demo --trace data/sample.csv
# Final realized PnL 6.00; flat position.
~/build/trading-engine-core/trading_demo --trace --contract-demo data/sample.csv
# Synthetic tick 0.25, multiplier 10; final realized PnL 60.00; flat position.
ctest --test-dir ~/build/trading-engine-core -R Instruments --output-on-failure -V
```

The catalog tests cover a second synthetic instrument with tick 0.1, multiplier 5,
as well as long/short reversal, multiple symbols, loss/notional boundaries,
off-grid order/quote/fill reports, and replacement behavior. See VERIFICATION.md
for the actual completed test count and checks; do not claim unrun sanitizer/CI jobs.

## Next proposed increment

Replay throughput measurements. Define and report the measured boundary explicitly
(CSV parsing alone versus the full producer/queue/consumer path). Use generated
synthetic input, disable tracing, report tick count, elapsed time, ticks/second,
build type/compiler and queue capacity, and preserve deterministic correctness
checks. Do not claim exchange latency or production performance from replay timing.
Engine order/fill/ID history is retained, so separate it from CSV input memory use.
Agree scope before broadening into live feeds, networking, or new strategies.

## Collaboration and saving

Give clear progress messages and an explicit completion message. If user input is
needed, end the response with a clear question. Do not ask routine confirmations
when already authorized. When accumulated context makes continuation unreliable,
update this handoff with concrete completed/pending work, then tell the user it is
time to continue in a new session. Do not create a new chat automatically.

Use meaningful English feature commits. /save invokes .agents/skills/save/SKILL.md:
inspect the actual changes and existing unpushed commits, reuse valid tests when
source is unchanged, and perform a normal push. Do not force-push or duplicate an
already-created commit. Ordinary implementation work ends with a local commit;
report its actual push state explicitly.
