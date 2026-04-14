#!/usr/bin/env bash
set -euo pipefail

# PGO build script for koneko simulators
# Usage: ./scripts/pgo-build.sh [--lto] [--bolt]

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIM="$ROOT/sim"
BUILD="$SIM/build"
PAYLOADS="$SIM/payloads"
PGO_DIR="$BUILD/pgo-profiles"

USE_LTO=OFF
USE_BOLT=false

for arg in "$@"; do
  case "$arg" in
    --lto) USE_LTO=ON ;;
    --bolt) USE_BOLT=true ;;
  esac
done

echo "=== PGO Build Pipeline ==="
echo "LTO: $USE_LTO  BOLT: $USE_BOLT"

# Step 1: Instrumented build
echo ""
echo "--- Step 1: Building with PGO instrumentation ---"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

cmake -GNinja "$SIM" \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DPGO_GENERATE=ON \
  -DPGO_PROFILE_DIR="$PGO_DIR"

ninja -j$(nproc) sim_single sim_soft sim_system

# Step 2: Profile workloads
echo ""
echo "--- Step 2: Running profile workloads ---"
mkdir -p "$PGO_DIR"

echo "  Profiling sim_single (sha256)..."
"$BUILD/sim_single" "$PAYLOADS/c/sha256_test.bin" --max-cycles 10000000 \
  >/dev/null 2>&1 || true

echo "  Profiling sim_soft (random_noc)..."
"$BUILD/sim_soft" "$PAYLOADS/sys/random_noc_test.bin" --max-cycles 200000 \
  >/dev/null 2>&1 || true

echo "  Profiling sim_system (random_noc)..."
"$BUILD/sim_system" "$PAYLOADS/sys/random_noc_test.bin" --soft --max-cycles 200000 \
  >/dev/null 2>&1 || true

# Step 3: Merge profiles
echo ""
echo "--- Step 3: Merging profiles ---"
llvm-profdata merge -output="$PGO_DIR/default.profdata" "$PGO_DIR"/*.profraw
echo "  Merged $(ls "$PGO_DIR"/*.profraw | wc -l) profile files"

# Step 4: Optimized build
echo ""
echo "--- Step 4: Building with PGO optimization ---"
rm -rf "$BUILD/CMakeCache.txt" "$BUILD/CMakeFiles" "$BUILD/build.ninja"

cmake -GNinja "$SIM" \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DPGO_USE=ON \
  -DPGO_PROFILE_DIR="$PGO_DIR" \
  -DUSE_LTO="$USE_LTO"

ninja -j$(nproc) sim_single sim_soft sim_system

# Step 5 (optional): BOLT
if [ "$USE_BOLT" = true ]; then
  echo ""
  echo "--- Step 5: BOLT optimization ---"
  for bin in sim_single sim_soft sim_system; do
    echo "  BOLTing $bin..."
    cp "$BUILD/$bin" "$BUILD/${bin}.prebolt"

    # Collect perf data
    if [ "$bin" = "sim_single" ]; then
      perf record -e cycles:u -o "$BUILD/${bin}.perf.data" -- \
        "$BUILD/${bin}.prebolt" "$PAYLOADS/c/sha256_test.bin" --max-cycles 5000000 \
        >/dev/null 2>&1 || true
    else
      PAYLOAD="$PAYLOADS/sys/random_noc_test.bin"
      perf record -e cycles:u -o "$BUILD/${bin}.perf.data" -- \
        "$BUILD/${bin}.prebolt" "$PAYLOAD" --soft --max-cycles 100000 \
        >/dev/null 2>&1 || true
    fi

    perf2bolt -p "$BUILD/${bin}.perf.data" -o "$BUILD/${bin}.fdata" \
      "$BUILD/${bin}.prebolt" 2>/dev/null || {
        echo "    BOLT: perf2bolt failed for $bin, skipping"
        mv "$BUILD/${bin}.prebolt" "$BUILD/$bin"
        continue
      }

    llvm-bolt "$BUILD/${bin}.prebolt" -o "$BUILD/$bin" \
      -data="$BUILD/${bin}.fdata" \
      -reorder-blocks=ext-tsp \
      -reorder-functions=hfsort \
      -split-functions \
      -split-all-cold \
      -dyno-stats 2>/dev/null || {
        echo "    BOLT: llvm-bolt failed for $bin, keeping pre-BOLT binary"
        mv "$BUILD/${bin}.prebolt" "$BUILD/$bin"
      }
  done
fi

# Step 6: Benchmark
echo ""
echo "=== Final Benchmarks ==="
echo "--- sim_single (sha256, 10M cycles) ---"
"$BUILD/sim_single" "$PAYLOADS/c/sha256_test.bin" --max-cycles 10000000 2>&1 | grep -E "Speed|Runtime"

echo "--- sim_soft (random_noc, 200k cycles) ---"
"$BUILD/sim_soft" "$PAYLOADS/sys/random_noc_test.bin" --max-cycles 200000 2>&1 | grep -E "Speed|Runtime"

echo "--- sim_system (random_noc, 200k cycles) ---"
"$BUILD/sim_system" "$PAYLOADS/sys/random_noc_test.bin" --soft --max-cycles 200000 2>&1 | grep -E "Speed|Runtime"

echo ""
echo "=== Done ==="
