#!/usr/bin/env bash

set -e

BASE=$(readlink -f $(dirname $(readlink -f $0))/../..)
WORKDIR=$(readlink -f $1)/$2
mkdir -p $WORKDIR

echo "Workdir: $1/$2" >&1

cd $WORKDIR
# git lfs pull

export MEOW_CORE_CNT=$2

function work() {
  cp -r $BASE/sim/payloads $WORKDIR/text
  cd text
  make CORE_CNT=$MEOW_CORE_CNT
  cd ..

  export MEOW_DATA=$WORKDIR/data
  export MEOW_MEM=${AN_DRAMSIM_CONF:-$BASE/sim/mem.cfg}

  export MEOW_LOG=
  export MEOW_TRACE=

  mkdir -p $MEOW_DATA
  $BASE/work/bin/datagen --core-cnt $MEOW_CORE_CNT --tot-neuron 100000 --connectivity 0.1 --pre-simulate 95 --tau 0.12 --dump ./data
  echo "======= Data generator: Ret value: $?"

  export MEOW_TEXT=$WORKDIR/text/test.single.bin
  export MEOW_MEM_LOG="$WORKDIR/single/dram"
  mkdir -p $MEOW_MEM_LOG
  export MEOW_MEM_CNT=1
  $BASE/work/bin/sim.single | tee $WORKDIR/single/log.txt
  echo "======= Simulator (single dram): Ret value: $?"

  export MEOW_TEXT=$WORKDIR/text/test.double.bin
  export MEOW_MEM_LOG="$WORKDIR/double/dram"
  mkdir -p $MEOW_MEM_LOG
  export MEOW_MEM_CNT=2
  $BASE/work/bin/sim.double | tee $WORKDIR/double/log.txt
  echo "======= Simulator (double dram): Ret value: $?"
}

work | tee $WORKDIR/log.full.txt
