# Verification

Verified on 2026-09-29.

| Check | Result |
| --- | --- |
| CMake Debug configure and build | Passed |
| Compiler | GCC 15.2.0, x86_64 Windows, w64devkit 2.5.0 |
| Build system | CMake 3.31.8, MinGW Makefiles |
| GoogleTest | 1.15.2, source archive verified against the hash in CMakeLists.txt |
| GoogleTest cases | 37 passed |
| CTest demo smoke checks | 2 passed |
| Overall CTest | 39/39 passed, 0 failed |
| Built-in input and CSV replay | Both produced 2 filled orders and realized PnL 6.00 |
| Missing replay file | Reported an error and returned exit code 1 |
| Final project build diagnostics | No compiler warnings after fixing a discarded nodiscard result in a test |
| Native Linux / Ubuntu run | Not run: the host has no installed WSL/Linux environment |
| ASan / UBSan | Not run locally; configured in the Ubuntu Clang CI job |
| Remote GitHub Actions | Not run; repository has not been published |

The local build used the official portable
[w64devkit release](https://github.com/skeeto/w64devkit/releases/tag/v2.5.0) and
[CMake release](https://github.com/Kitware/CMake/releases/tag/v3.31.8).
Both downloads matched their published SHA-256 digests. Tools were extracted
outside this repository and are excluded from the source archive.

Local artifacts are in `build/`: `trading_demo.exe`, `trading_tests.exe`,
`test-results.xml`, and `Testing/Temporary/LastTest.log`. They are ignored by Git
and excluded from the source archive. This report records a Windows GCC run;
it does not assert a completed Ubuntu or sanitizer run. Follow the README to
reproduce on Ubuntu or push to GitHub to execute the included CI workflow.

Suggested initial commits, after staging the corresponding files:

1. `feat: implement deterministic trading engine vertical slice`
2. `test: cover risk execution pnl replay and queue shutdown`
3. `ci: add Ubuntu builds and sanitizer checks`
4. `docs: document architecture build steps and verification`
