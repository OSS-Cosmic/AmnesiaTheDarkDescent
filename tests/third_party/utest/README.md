# Vendored utest.h

This directory contains an exact, offline vendoring of `sheredom/utest.h` at
upstream commit `fb622dc480a56cc53ac9562a4436281bef91c989` (2025-08-30,
`Agg init for tests with old VS`). The source is the single-header framework;
the adjacent `LICENSE` is the upstream Unlicense text.

Provenance:

- Header: <https://github.com/sheredom/utest.h/blob/fb622dc480a56cc53ac9562a4436281bef91c989/utest.h>
- License: <https://github.com/sheredom/utest.h/blob/fb622dc480a56cc53ac9562a4436281bef91c989/LICENSE>
- `utest.h` SHA-256: `f20f27a4fa3f8a32e5044f5609ffcd2407db72bbb55afcb676f873c756988bf4`
- `LICENSE` SHA-256: `7e12e5df4bae12cb21581ba157ced20e1986a0508dd10d0e8a4ab9a4cf94e85c`

The test build includes this directory locally; it never downloads test
dependencies at build time.

## Usage in this tree

Include `utest.h` and define tests with `UTEST(suite, name)` at global scope.
The generated declarations and registrations must match the runner; an
anonymous namespace can cause linker errors. Exactly one translation unit in
each utest executable owns the runner, normally with `UTEST_MAIN()`. A project
that needs custom process setup may use `UTEST_STATE()` in one translation unit
and call `utest_main(argc, argv)` from its own `main`. Do not define either
runner more than once.

Names are `suite.name`, and `--list-tests` prints names suitable for
`--filter=<name>`. `--random-order[=<seed>]` is useful for finding accidental
test coupling; `--help` lists all runner options, including `--output` and
`--enable-mixed-units`.

Fixtures use `UTEST_F_SETUP(Type)`, `UTEST_F(Type, name)`, and
`UTEST_F_TEARDOWN(Type)`. The pinned macros zero-initialize each fixture with
`memset` before setup, so fixture types must contain only plain,
zero-initializable fields; nontrivial C++ members with constructors or
destructors are unsafe. Put RAII objects local to test bodies. Setup failure
returns before teardown, so setup must explicitly roll back anything acquired
before a fatal assertion or other failure. After successful setup, teardown
runs after the body even when the body fails. Use teardown for external
resources not owned by a test-body RAII object. Keep fixtures independent and
do not rely on test ordering.

Use `ASSERT_*` for a failure that makes the remainder of the current test
invalid and `EXPECT_*` when later checks remain meaningful. `ASSERT_NEAR` and
`EXPECT_NEAR` mean absolute difference less than or equal to epsilon and reject
NaN. Preserve numeric tolerances exactly during migrations, and preserve the
original predicate for relative or strict comparisons when `*_NEAR` is not
equivalent. Integer, enum, pointer, string, and byte comparisons should use
the corresponding exact assertions.

In this repository, only `BindlessPoolTests` was converted to utest. Other C++
suites retain their existing runners; `premake/tests.lua` merely adds this
include directory to every C++ test project. Third-party suites are out of
scope. From the repository root, the converted suite can be exercised with:

```sh
./build-premake/tests/Debug/BindlessPoolTests --list-tests
./build-premake/tests/Debug/BindlessPoolTests --filter=LRUCache.TailHitDoesNotOrphanSlots
./build-premake/tests/Debug/BindlessPoolTests --random-order=1234
```
