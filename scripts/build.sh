#!/usr/bin/env bash

set -e

BASE=$(readlink -f $(dirname $(readlink -f $0))/..)
echo "Building inside $BASE..."

cd $BASE

build_sim() {
  cd $BASE

  # Elaborate RTL, generate SystemVerilog of the core
  mill Koneko.run

  # Verilate SystemVerilog to C++, and build simulator
  rm -rf work/build/sim
  mkdir -p work/build/sim
  cd work/build/sim
  cmake -GNinja $BASE/sim
  ninja
}

build_datagen() {
  cd $BASE/datagen
  cargo build --release
}

rm -rf work/bin
mkdir -p work/bin
AN_PIPE_CNT=1 build_sim && cp $BASE/work/build/sim/sim $BASE/work/bin/sim.single
AN_PIPE_CNT=2 build_sim && cp $BASE/work/build/sim/sim $BASE/work/bin/sim.double

# Build datagen
build_datagen && cp $BASE/datagen/target/release/datagen $BASE/work/bin/datagen

# Try building payload
cd $BASE/sim/payloads
make
make clean
