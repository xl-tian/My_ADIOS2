#!/usr/bin/env bash
#
# Build and run the ADIOS2 engine write thread-safety test.
#
# Assumes ADIOS2 has been built (BP5, serial) in $ADIOS2_BUILD. For a
# deterministic verdict that build should be compiled with ThreadSanitizer:
#
#   cmake -S <adios2-src> -B build-tsan \
#         -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=ON \
#         -DADIOS2_USE_MPI=OFF -DADIOS2_USE_Fortran=OFF -DADIOS2_USE_Python=OFF \
#         -DADIOS2_USE_SST=OFF -DADIOS2_USE_DataMan=OFF \
#         -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1" \
#         -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
#         -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
#         -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
#   cmake --build build-tsan --target adios2_cxx -j
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
ADIOS2_BUILD="${ADIOS2_BUILD:-$SRC/build-tsan}"

# ThreadSanitizer on by default (matches the recommended library build above).
TSAN="${TSAN:-1}"
SAN_FLAGS=""
if [[ "$TSAN" == "1" ]]; then
    SAN_FLAGS="-fsanitize=thread"
fi

INCLUDES=(
    -I"$SRC/bindings/CXX"
    -I"$SRC/source"
    -I"$ADIOS2_BUILD/source"
)

echo ">> Compiling concurrent_put_test ..."
g++ -std=c++17 -g -O1 $SAN_FLAGS "${INCLUDES[@]}" \
    "$HERE/concurrent_put_test.cpp" \
    -L"$ADIOS2_BUILD/lib" -ladios2_cxx -ladios2_core \
    -Wl,-rpath,"$ADIOS2_BUILD/lib" \
    -pthread \
    -o "$HERE/concurrent_put_test"

export LD_LIBRARY_PATH="$ADIOS2_BUILD/lib:${LD_LIBRARY_PATH:-}"
# Don't let the first race report abort the run; we want to see the result line.
export TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 ${TSAN_OPTIONS:-}"

run() {
    local label="$1"; shift
    echo
    echo "############################################################"
    echo "## $label"
    echo "############################################################"
    set +e
    "$HERE/concurrent_put_test" "$@"
    local rc=$?
    set -e
    echo "(exit code: $rc)"
    return 0
}

# 1) Serial baseline  -> must PASS, TSan-clean.
run "SERIAL baseline (control)"            --serial     --iters 5

# 2) Mutex-guarded    -> must PASS, TSan-clean. Shows the ops are fine when serialized.
run "MUTEX-guarded concurrency (control)"  --mutex      --iters 5

# 3) Concurrent, no lock -> the case under test. Expect TSan race reports and/or
#    read-back corruption / exceptions.
run "CONCURRENT, no lock (CLAIM UNDER TEST)" --concurrent --threads 8 --iters 20

echo
echo "Done. See README.md for how to read these results."
