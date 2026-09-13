#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/fluid-studios-test.XXXXXX")
trap 'rm -rf "$build"' EXIT INT TERM

cc_bin=${CC:-cc}
cxx_bin=${CXX:-c++}
timeout_bin=${TIMEOUT:-timeout}
common="-std=c++17 -pthread -DMMGR_TESTING"
test_args_display=$*
show_output=false

if [ "$#" -gt 0 ]; then
	for arg do
		case $arg in --list-tests|--help) show_output=true ;; esac
		case $arg in
			--filter=*|--list-tests|--random-order|--random-order=*|--help)
				;;
			*)
				printf '%s\n' "unsupported FluidStudios test option: $arg" >&2
				exit 2
				;;
		esac
	done
fi

run_one() {
	name=$1; shift
	flags=
	while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do
		flags="$flags $1"
		shift
	done
	[ "$#" -gt 0 ] && shift
	$cc_bin -std=gnu11 -DMMGR_TESTING $flags -c "$root/HPL2/extern/FluidStudios/MemoryManager/mmgr.c" -o "$build/$name-mmgr.o"
	$cxx_bin $common $flags -I"$root/HPL2/extern/FluidStudios/MemoryManager" \
		-I"$root/tests/third_party/utest" "$root/tests/memory/fluid_studios_platform_test.cpp" \
		"$root/HPL2/extern/FluidStudios/MemoryManager/mmgr_platform.cpp" "$build/$name-mmgr.o" \
		-o "$build/$name"
	log="$build/$name.log"
	printf '%s\n' "FluidStudiosMemoryTests $name: running${test_args_display:+ $test_args_display} (log: $log)"
	set +e
	FLUID_STUDIOS_TEST_ROOT="$build/$name-files" "$timeout_bin" "${TEST_TIMEOUT:-30}s" "$build/$name" "$@" >"$log" 2>&1
	status=$?
	set -e
	if [ "$status" -ne 0 ]; then
		printf '%s\n' "FluidStudiosMemoryTests $name failed (status $status); captured output:"
		cat "$log"
		return "$status"
	fi
	if [ "$show_output" = true ]; then cat "$log"; fi
	printf '%s\n' "FluidStudiosMemoryTests $name passed"
}

for label in debug release; do
	build_flags=""
	if [ "$label" = release ]; then build_flags=-DNDEBUG; fi
	for backtrace in 0 1; do
		run_one "${label}-bt${backtrace}" $build_flags -DMMGR_BACKTRACE=$backtrace -- "$@"
	done
done

# Sanitizers are optional toolchain features. Compile a probe so a compiler
# without them skips this part instead of making the Linux runner unusable.
if printf 'int main(void){return 0;}\n' | $cxx_bin -x c++ -fsanitize=address,undefined -o "$build/sanitizer-probe" - >/dev/null 2>&1; then
	run_one sanitizer -DMMGR_BACKTRACE=0 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -- "$@"
fi
