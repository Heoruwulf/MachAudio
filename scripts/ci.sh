#!/bin/bash
set -e

echo "=== MachAudio CI Pipeline ==="

# Check for required tools
if ! command -v clang &> /dev/null; then
    echo "clang could not be found. Please install it."
    exit 1
fi

echo "--- Phase 1: Static Analysis (scan-build) ---"
if command -v scan-build &> /dev/null; then
    rm -rf build_analyze
    mkdir -p build_analyze
    cd build_analyze
    export CC=clang
    scan-build cmake .. -DCMAKE_BUILD_TYPE=Debug
    scan-build -status-bugs make -j$(nproc)
    cd ..
else
    echo "scan-build not found. Skipping static analysis phase."
fi

echo "--- Phase 2: Fast Path (ASan, UBSan, Unit Tests) ---"
rm -rf build
mkdir -p build
cd build
export CC=clang
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
ctest --output-on-failure
cd ..

echo "--- Phase 3: Parser Hardening (libFuzzer) ---"
rm -rf build_fuzz
mkdir -p build_fuzz
cd build_fuzz
export CC=clang
cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_FUZZERS=ON
make -j$(nproc) fuzz_protocol
echo "Running fuzzer for 5 seconds..."
./bin/fuzz_protocol -max_total_time=5
cd ..

echo "=== CI Pipeline Passed ==="
