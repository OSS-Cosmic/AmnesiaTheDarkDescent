# FluidStudios memory manager backend

This directory vendors the four files from The-Forge revision `a9713f13f6650bdd38b4d8bf0e2c5fd5a4140372`:

Original source retrieval (pinned revision):
[The-Forge FluidStudios source](https://github.com/ConfettiFX/The-Forge/tree/a9713f13f6650bdd38b4d8bf0e2c5fd5a4140372/Common_3/Utilities/ThirdParty/OpenSource/FluidStudios/MemoryManager)

| file | SHA-256 |
| --- | --- |
| `mmgr.c` | `7383b9937984b93cf61d74ef59ab6c9eb00e1dda93b2c634fc657df8b6d153db` |
| `mmgr.h` | `aaf0c29ab5972469bfe016437a974b23044c1349badad2f0d41b019879bee844` |
| `nommgr.h` | `bd2666ef49df740ab0a6c797d2920834283267fbc2a90726f5d8f0df1a06011d` |
| `readme.txt` | `38f12e6f75084416ba2c5d1557bb3aa77a4adc93c9e6f9118b209d0ac6239602` |

The original Fluid Studios copyright, author credit, usage restrictions, and source-obtain instructions remain in the vendored files. The source is not relicensed by this repository.

Local adaptations include portability, lifecycle safety, and allocation hardening: Forge OS/log/filesystem/thread APIs were replaced by `mmgr_platform.cpp`; byte totals, identifiers, and counts use `size_t`; arithmetic is checked before publishing allocations; realloc uses replacement storage and commits only after allocation, copying, and accounting succeed. Guards and fill operations preserve exact requested lengths. Invalid ownership, family mismatches, and corrupted guards are diagnosed without freeing the allocation, independently of assertions. Configuration strings and event logs use bounded storage, Vulkan leak suppression and leak-count assertions were removed, and reports do not free tracking state. `MMGR_BACKTRACE=0` is honored by both header and implementation.

Alignment must be a power of two at least `sizeof(void*)`; valid requests are promoted to at least `max_align_t` alignment. Zero-byte allocation returns a freeable non-null pointer on success. `realloc(NULL,n)` allocates, and `realloc(p,0)` frees a valid C-family allocation and returns null. Failed nonzero realloc preserves its original bytes, guards, ownership, and all statistics. Accumulated totals increase on growth and do not decrease on shrink. Address validation accepts exact allocation bases only.

`mmgr_platform.cpp` exposes only a private `extern "C"` API. It uses process-lifetime native locks (lock 0 for allocation bookkeeping, lock 1 for diagnostic logging), CRT file I/O, and fixed buffers; it does not use HPL APIs, STL, or global `new`. `exitMemAlloc` emits a report when initialized but does not shut down or invalidate the manager.

Reports resolve relative filenames beneath `mmgrSetLogFileDirectory`; absolute filenames remain absolute. Report and statistics reads hold the allocation lock for a consistent snapshot, and the lock order is allocation then logging. Report writes, flushes, and closes report failures to stderr. Windows symbols fall back to `<unknown>` and the raw address when the symbol service cannot resolve a frame. Configuration uses safe locked setters that copy values; the old pointer-to-boolean option APIs are not used.

Repeatable Linux validation from the repository root (the runner applies a
30-second timeout to each executable):

```sh
tests/memory/run_fluid_studios_tests.sh
```

The runner covers Debug and `NDEBUG` builds with `MMGR_BACKTRACE=0` and `1`,
and adds an AddressSanitizer/UndefinedBehaviorSanitizer build when the local
compiler supports those flags. It is intentionally standalone and does not
fetch packages.

The backend still depends on the platform's native CRT/POSIX or Win32 debug-symbol facilities; symbolized backtraces are therefore best-effort, and report output can be incomplete if the destination rejects writes. The original manager also retains fixed-size owner-name and application-name fields.

Premake provides `FluidStudiosMemoryTests` and `FluidStudiosMemoryTestsNoBacktrace`, each with post-build execution and `MMGR_TESTING` enabled. The standalone suite checks first-use concurrency; raw, empty-reservoir, pool-list, report, and realloc failure stages; full rollback payload/ownership/statistics; exact sizes and guards; zero-size and realloc contracts; ordinary and extended alignment plus invalid alignment and overflow; invalid, interior, double, and mismatched frees/reallocations; prepended hash-chain removal; synthetic accounting beyond 4 GiB; and bounded concurrent allocation, realloc/free, statistics, validation, and reporting. Reports are checked without treating client payload scanning as a valid implementation strategy.

The standalone suite is intended to cover final hardening: first-use and
four-worker stress, transactional realloc failure and shrink semantics,
monotonic realloc accumulation, exact odd-size guards, family/type rejection,
calloc zero fill, moved and hash-chain reallocations, and isolated report
paths. Unused-byte queries are intentionally made only while the client is
quiescent, because they inspect payload bytes. The standalone backend suite
passed Debug/Release with backtraces off/on and ASan/UBSan with
`ASAN_OPTIONS=detect_leaks=0`; LeakSanitizer cannot run its process scan in this
harness. The canonical application builds were blocked by Docker socket
permission denied, and `premake5` is absent. Windows compilation/runtime
(including `dbghelp.lib`) remain unverified.

This remains a standalone backend test, while the approved HPL rollout uses the
same tracker for HPL allocation macros and global replaceable C++ operators
only when `--memory-tracking=yes`; the default build remains disabled. The
rollout maps each log filename to that filename plus `.memreport` (for example,
`hpl.log.memreport`). The contract includes safe first use before explicit
initialization; applications should still initialize and select the report
directory during startup.
`MMGR_TESTING` and `mmgr_test.h` are private test seams and must not be enabled
or shipped in production configurations.
