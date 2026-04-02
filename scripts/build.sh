#!/usr/bin/env bash

set -e

BASE=$(readlink -f $(dirname $(readlink -f $0))/..)
echo "Building inside $BASE..."

cd $BASE

build_sim_single() {
  cd $BASE

  # Elaborate RTL, generate SystemVerilog of the core
  mill Koneko.run

  # Verilate SystemVerilog to C++, and build simulator
  rm -rf work/build/sim
  mkdir -p work/build/sim
  cd work/build/sim
  cmake -GNinja $BASE/sim
  ninja sim_single
}

build_sim_system() {
  cd $BASE
  local pu=${1:-16}
  local mc=${2:-1}

  # Elaborate RTL, generate SystemVerilog of the System module
  AN_SYSTEM=1 AN_NUM_PU=$pu AN_NUM_MC=$mc AN_PIPE_CNT=1 mill Koneko.run

  # Verilate SystemVerilog to C++, and build simulator
  rm -rf work/build/sim_system
  mkdir -p work/build/sim_system
  cd work/build/sim_system
  cmake -GNinja $BASE/sim
  ninja sim_system
}

build_datagen() {
  cd $BASE/datagen
  cargo build --release
}

rm -rf work/bin
mkdir -p work/bin
AN_PIPE_CNT=1 build_sim_single && cp $BASE/work/build/sim/sim_single $BASE/work/bin/sim_single
build_sim_system ${AN_NUM_PU:-16} ${AN_NUM_MC:-1} && cp $BASE/work/build/sim_system/sim_system $BASE/work/bin/sim_system

# Build datagen
build_datagen && cp $BASE/datagen/target/release/datagen $BASE/work/bin/datagen

# Build the current payloads/tests
cd $BASE/sim/payloads
make sys/snn_main.bin
make sys-tests all-tests
make clean
