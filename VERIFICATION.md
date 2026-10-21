# Verification

Verified on 2026-10-21 (Asia/Seoul), based on commit ebfcdd9 plus the instrument metadata increment.

| Check | Result |
| --- | --- |
| Platform | Ubuntu 24.04 under WSL |
| Compiler | GCC 13.3.0 |
| CMake | 3.28.3 |
| Build | Debug, out-of-tree build at ~/build/trading-engine-core |
| GoogleTest cases | 108 passed, including 15 instrument metadata tests |
| CTest demo checks | 15 passed, including the contract multiplier demo |
| Overall CTest | 123/123 passed, 0 failed |
| Final build diagnostics | No compiler warnings emitted after constructor/fixture cleanup |
| Whitespace validation | git diff --check passed |
| ASan / UBSan | Not run for this increment |
| Remote GitHub Actions | Not run for this increment |

All earlier order, risk, cancellation, replacement, execution ID, queue, CSV,
position/PnL, and shutdown tests pass. New instrument tests exercise:

- Unknown and duplicate symbols, invalid tick sizes and multipliers.
- Constant tick grids, decimal representation tolerance, supported price bounds.
- Quote and fill rejection without position mutation; off-grid midpoints/averages.
- Long and short reversals, reductions, closures, and per-symbol multipliers.
- Market buy/sell and limit order notional boundaries, and scaled session loss.
- Order validation, execution ID reuse after a rejected report, and deduplication.
- Engine round-trip accounting and preserved execution IDs.
- Invalid replacements preserving the original order; valid replacements rechecking
  scaled notional after cancellation.
- Invalid quote/fill integration reports stopping the run with existing cleanup.

Manual contract demo using data/sample.csv produced:

```text
instrument=SYNTH tick_size=0.25 contract_multiplier=10.00
ticks=4 orders=2 fills=2
order=1 state=Filled filled=2 average_price=101.00
order=2 state=Filled filled=2 average_price=104.00
SYNTH position=0 realized_pnl=60.00 unrealized_pnl=0.00
```

The default CSV demo still returns realized PnL 6.00. Only synthetic data and
clean-room components are used; these are not real exchange specifications.

Reproduce from Ubuntu:

```sh
cd /mnt/c/tmp/signaltrue_home/dev/Globalquant_trading/trading-engine-core
cmake -S . -B ~/build/trading-engine-core
cmake --build ~/build/trading-engine-core --parallel 2
ctest --test-dir ~/build/trading-engine-core --output-on-failure
~/build/trading-engine-core/trading_demo --contract-demo data/sample.csv
```

Prices remain double with a constant positive tick grid and at most 1e12 ticks per
price. All instruments share one accounting unit; fees, FX, settlement, variable
ladders, negative prices, derivatives margin, and Greeks are not modeled.
PositionManager does not deduplicate independently; OrderManager remains the gate.
Deduplication is per run in memory, with no restart recovery or trade corrections.
Streaming input retains no tick history, but engine order/fill/ID history grows.
No throughput or memory benchmark was performed for this increment.

See WORK_STATUS.md for session handoff, collaboration preferences, and next work.
