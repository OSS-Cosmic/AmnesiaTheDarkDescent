#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/hpl-memory-test.XXXXXX")
trap 'rm -rf "$build"' EXIT INT TERM
cd "$build"
cc_bin=${CC:-cc}; cxx_bin=${CXX:-c++}; timeout_bin=${TIMEOUT:-timeout}
inc="-I$root/HPL2/core/include -I$root/HPL2/extern/FluidStudios/MemoryManager -I$root/tests/memory -I$root/tests/third_party/utest"
common="-std=c++17 -pthread -fexceptions $inc -DMEMORY_MANAGER_ACTIVE -DMMGR_TESTING"
$cxx_bin $common -fno-exceptions -c "$root/HPL2/core/sources/system/MemoryManager.cpp" -o "$build/active-facade-no-exceptions.o"
# Clang and recent GCC support this flag; older compilers may not.
observe_new_flags=""
if printf 'int main() { return 0; }\n' | $cxx_bin -std=c++17 -x c++ -fno-assume-sane-operators-new-delete -c -o "$build/new-flag-probe.o" - >/dev/null 2>&1; then
    observe_new_flags="-fno-assume-sane-operators-new-delete"
fi
run_one() {
    name=$1; shift; run_flags=$*
    $cc_bin -std=gnu11 -DMMGR_TESTING $run_flags -c "$root/HPL2/extern/FluidStudios/MemoryManager/mmgr.c" -o "$build/$name-mmgr.o"
$cxx_bin $common $run_flags $observe_new_flags "$root/tests/memory/hpl_memory_manager_test.cpp" "$root/tests/memory/hpl_memory_manager_fixture_before_main.cpp" "$root/tests/memory/hpl_memory_manager_fixture_after_report.cpp" "$root/tests/memory/hpl_memory_manager_lifetime_tests.cpp" "$root/tests/memory/hpl_memory_manager_macro_contract_test.cpp" "$root/tests/memory/hpl_memory_manager_attribution_tests.cpp" "$root/tests/memory/hpl_memory_log_stub.cpp" "$root/HPL2/core/sources/system/MemoryManager.cpp" "$root/HPL2/core/sources/memory/MemoryOperators.cpp" "$root/HPL2/extern/FluidStudios/MemoryManager/mmgr_platform.cpp" "$build/$name-mmgr.o" -o "$build/$name"
    symbols=$(nm -C --defined-only "$build/$name")
    for signature in 'operator new(unsigned long)' 'operator new[](unsigned long)' 'operator new(unsigned long, std::nothrow_t const&)' 'operator new[](unsigned long, std::nothrow_t const&)' 'operator delete(void*)' 'operator delete[](void*)' 'operator delete(void*, unsigned long)' 'operator delete[](void*, unsigned long)' 'operator new(unsigned long, std::align_val_t)' 'operator new[](unsigned long, std::align_val_t)' 'operator new(unsigned long, std::align_val_t, std::nothrow_t const&)' 'operator new[](unsigned long, std::align_val_t, std::nothrow_t const&)' 'operator delete(void*, std::align_val_t)' 'operator delete[](void*, std::align_val_t)' 'operator delete(void*, unsigned long, std::align_val_t)' 'operator delete[](void*, unsigned long, std::align_val_t)' 'operator delete(void*, std::nothrow_t const&)' 'operator delete[](void*, std::nothrow_t const&)' 'operator delete(void*, std::align_val_t, std::nothrow_t const&)' 'operator delete[](void*, std::align_val_t, std::nothrow_t const&)'; do
        count=$(printf '%s\n' "$symbols" | awk -v signature="$signature" '{ line=$0; sub(/^[^[:space:]]+[[:space:]]+[^[:space:]]+[[:space:]]+/, "", line); if (line == signature) count++ } END { print count + 0 }'); [ "$count" -eq 1 ] || { echo "linked operator symbol '$signature' count=$count" >&2; exit 1; }
    done
    mkdir -p "$build/$name-root"; HPL_MEMORY_TEST_ROOT="$build/$name-root" "$timeout_bin" "${TEST_TIMEOUT:-30}s" "$build/$name"
    if [ "$name" = debug-bt0 ]; then
        list=$(HPL_MEMORY_TEST_ROOT="$build/$name-root" "$timeout_bin" "${TEST_TIMEOUT:-30}s" "$build/$name" --list-tests)
        printf '%s\n' "$list" | grep -Fx 'HplMemoryManager.Attribution' >/dev/null
        printf '%s\n' "$list" | grep -Fx 'HplMemory.Facade' >/dev/null
        printf '%s\n' "$list" | grep -Fx 'HplMemory.Attribution' >/dev/null
        printf '%s\n' "$list" | grep -Fx 'HplMemory.Realloc' >/dev/null
        printf '%s\n' "$list" | grep -Fx 'HplMemory.Operators' >/dev/null
        printf '%s\n' "$list" | grep -Fx 'HplMemory.ConcurrencyAndLifetimes' >/dev/null
        printf '%s\n' "$list" | grep -Fx 'HplMemory.Reports' >/dev/null
        printf '%s\n' "$list" | grep -Fx 'HplMemory.MacroContracts' >/dev/null
        printf '%s\n' "$list" | grep -Fx 'HplNativeLifetimes.SmartAndStl' >/dev/null
        printf '%s\n' "$list" | grep -Fx 'HplNativeLifetimes.ArraysAndAlignment' >/dev/null
        printf '%s\n' "$list" | grep -Fx 'HplNativeLifetimes.FailuresAndPlacement' >/dev/null
        printf '%s\n' "$list" | grep -Fx 'HplNativeLifetimes.VirtualAndClassSpecificDelete' >/dev/null
        printf '%s\n' "$list" | grep -Fx 'HplNativeLifetimes.SingleEvaluation' >/dev/null
        for test_name in \
            HplMemoryManager.Attribution HplMemory.Facade HplMemory.Attribution HplMemory.Realloc \
            HplMemory.Operators HplMemory.ConcurrencyAndLifetimes HplMemory.Reports \
            HplMemory.MacroContracts HplNativeLifetimes.SmartAndStl \
            HplNativeLifetimes.ArraysAndAlignment HplNativeLifetimes.FailuresAndPlacement \
            HplNativeLifetimes.VirtualAndClassSpecificDelete HplNativeLifetimes.SingleEvaluation; do
            HPL_MEMORY_TEST_ROOT="$build/$name-root" "$timeout_bin" "${TEST_TIMEOUT:-30}s" "$build/$name" --filter="$test_name" > "$build/filter-output.txt"
            grep -F '[  PASSED  ] 1 tests.' "$build/filter-output.txt" >/dev/null
        done
        HPL_MEMORY_TEST_ROOT="$build/$name-root" "$timeout_bin" "${TEST_TIMEOUT:-30}s" "$build/$name" --random-order=17 >/dev/null
    fi
    printf '%s\n' "HplMemoryManagerTests $name passed"
}
$cxx_bin -std=c++17 -fno-exceptions $inc -c "$root/tests/memory/hpl_memory_manager_disabled_compile_test.cpp" -o "$build/disabled-test.o"
$cxx_bin -std=c++17 -fno-exceptions $inc -c "$root/HPL2/core/sources/system/MemoryManager.cpp" -o "$build/disabled-facade.o"
$cxx_bin -std=c++17 -fno-exceptions $inc -c "$root/HPL2/core/sources/memory/MemoryOperators.cpp" -o "$build/disabled-operators.o"
$cxx_bin -std=c++17 -fno-exceptions $inc "$root/tests/memory/hpl_memory_manager_disabled_compile_test.cpp" "$root/tests/memory/hpl_memory_log_stub.cpp" "$root/HPL2/core/sources/system/MemoryManager.cpp" "$root/HPL2/core/sources/memory/MemoryOperators.cpp" -o "$build/disabled-facade"
if nm -u "$build/disabled-test.o" "$build/disabled-facade.o" "$build/disabled-operators.o" "$build/disabled-facade" 2>/dev/null | grep -E 'mmgr|Mmgr'; then echo 'disabled HPL memory facade unexpectedly references mmgr' >&2; exit 1; fi
"$timeout_bin" "${TEST_TIMEOUT:-30}s" "$build/disabled-facade"
disabled_list=$("$timeout_bin" "${TEST_TIMEOUT:-30}s" "$build/disabled-facade" --list-tests)
printf '%s\n' "$disabled_list" | grep -Fx 'HplMemoryManagerDisabled.StatisticsAndValidity' >/dev/null
printf '%s\n' "$disabled_list" | grep -Fx 'HplMemoryManagerDisabled.LogFlagAndAllocationsAreInert' >/dev/null
printf '%s\n' "$disabled_list" | grep -Fx 'HplMemoryManagerDisabled.ReportIsNoOp' >/dev/null
for test_name in \
    HplMemoryManagerDisabled.StatisticsAndValidity \
    HplMemoryManagerDisabled.LogFlagAndAllocationsAreInert \
    HplMemoryManagerDisabled.ReportIsNoOp; do
    "$timeout_bin" "${TEST_TIMEOUT:-30}s" "$build/disabled-facade" --filter="$test_name" > "$build/filter-output.txt"
    grep -F '[  PASSED  ] 1 tests.' "$build/filter-output.txt" >/dev/null
done
"$timeout_bin" "${TEST_TIMEOUT:-30}s" "$build/disabled-facade" --random-order=29 >/dev/null
$cxx_bin $common -c "$root/HPL2/core/sources/memory/MemoryOperators.cpp" -o "$build/operators.o"
symbols=$(nm -C --defined-only "$build/operators.o")
for signature in 'operator new(unsigned long)' 'operator new[](unsigned long)' 'operator new(unsigned long, std::nothrow_t const&)' 'operator new[](unsigned long, std::nothrow_t const&)' 'operator delete(void*)' 'operator delete[](void*)' 'operator delete(void*, unsigned long)' 'operator delete[](void*, unsigned long)' 'operator new(unsigned long, std::align_val_t)' 'operator new[](unsigned long, std::align_val_t)' 'operator new(unsigned long, std::align_val_t, std::nothrow_t const&)' 'operator new[](unsigned long, std::align_val_t, std::nothrow_t const&)' 'operator delete(void*, std::align_val_t)' 'operator delete[](void*, std::align_val_t)' 'operator delete(void*, unsigned long, std::align_val_t)' 'operator delete[](void*, unsigned long, std::align_val_t)' 'operator delete(void*, std::nothrow_t const&)' 'operator delete[](void*, std::nothrow_t const&)' 'operator delete(void*, std::align_val_t, std::nothrow_t const&)' 'operator delete[](void*, std::align_val_t, std::nothrow_t const&)'; do
    count=$(printf '%s\n' "$symbols" | grep -F "$signature" | wc -l); [ "$count" -eq 1 ] || { echo "operator symbol '$signature' count=$count" >&2; exit 1; }
done
for mode in debug release; do flags="-O0"; [ "$mode" = release ] && flags="-O2 -DNDEBUG"; for backtrace in 0 1; do run_one "$mode-bt$backtrace" "$flags -DMMGR_BACKTRACE=$backtrace"; done; done
