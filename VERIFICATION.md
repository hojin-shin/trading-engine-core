# Verification

Verified on 2026-10-21 (Asia/Seoul), based on commit 024ce38 plus the execution ID increment.

| Check | Result |
| --- | --- |
| Platform | Ubuntu 24.04 under WSL |
| Compiler | GCC 13.3.0 |
| CMake | 3.28.3 |
| Build | Debug, existing out-of-tree build at ~/build/trading-engine-core |
| GoogleTest cases | 85 passed, including 8 new execution ID tests |
| CTest demo checks | 14 passed |
| Overall CTest | 99/99 passed, 0 failed |
| Build diagnostics | No compiler warnings emitted |
| Whitespace validation | git diff --check passed |
| ASan / UBSan | Not run for this increment |
| Remote GitHub Actions | Not run for this increment |

The new tests exercise identical retransmissions within a batch and across ticks,
partial and full fills, retransmissions after cancellation, cancel/replace,
all six conflicting payload fields, zero IDs, invalid reports not consuming IDs,
deterministic simulator IDs, distinct IDs with identical economics, unique fill
counts, position/PnL accounting, and stopping on conflicting retransmissions.
Only synthetic data and clean-room simulator components are used.

Reproduce from Ubuntu:

```sh
cmake -S /mnt/c/tmp/signaltrue_home/dev/Globalquant_trading/trading-engine-core -B ~/build/trading-engine-core
cmake --build ~/build/trading-engine-core --parallel 2
ctest --test-dir ~/build/trading-engine-core --output-on-failure
```

Deduplication is in memory for one engine run. PositionManager relies on the
OrderManager result; it does not deduplicate independently. Restart recovery,
trade corrections/busts, and multi-venue execution identity remain out of scope.
