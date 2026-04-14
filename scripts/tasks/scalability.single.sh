#!/usr/bin/env bash

set -e

BASE=$(readlink -f $(dirname $(readlink -f $0))/../..)
WORKDIR=$(readlink -f $1)/$2
mkdir -p $WORKDIR

echo "Workdir: $1/$2" >&1

cd $WORKDIR
# git lfs pull

CORE_CNT=$2

function work() {
  cp -r $BASE/sim/payloads $WORKDIR/text
  cd text
  make CORE_CNT=$CORE_CNT
  cd ..

  local DATA_DIR=$WORKDIR/data
  local DRAM_CFG=${AN_DRAMSIM_CONF:-$BASE/sim/mem.cfg}

  mkdir -p $DATA_DIR
  $BASE/work/bin/datagen --core-cnt $CORE_CNT --tot-neuron 100000 --connectivity 0.1 --pre-simulate 95 --tau 0.12 --dump ./data
  echo "======= Data generator: Ret value: $?"

  local SINGLE_DRAM_LOG="$WORKDIR/single/dram"
  mkdir -p $SINGLE_DRAM_LOG
  # The double in sim_system means SMT = 2
  $BASE/work/bin/sim_system $WORKDIR/text/test.single.bin --soft --data $DATA_DIR --dram-config $DRAM_CFG --dram-log $SINGLE_DRAM_LOG | tee $WORKDIR/single/log.txt
  echo "======= Simulator (single dram): Ret value: $?"

  local DOUBLE_DRAM_LOG="$WORKDIR/double/dram"
  mkdir -p $DOUBLE_DRAM_LOG
  $BASE/work/bin/sim_system $WORKDIR/text/test.double.bin --soft --data $DATA_DIR --dram-config $DRAM_CFG --dram-log $DOUBLE_DRAM_LOG | tee $WORKDIR/double/log.txt
  echo "======= Simulator (double dram): Ret value: $?"

  rm -rf $DATA_DIR
}

work | tee $WORKDIR/log.full.txt
