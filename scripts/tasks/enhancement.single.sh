#!/usr/bin/env bash

set -e

BASE=$(readlink -f $(dirname $(readlink -f $0))/../..)
WORKDIR=$(readlink -f $1)/$2
mkdir -p $WORKDIR

echo "Workdir: $1/$2" >&1

cd $WORKDIR
# git lfs pull

TESTCASE=$2

function work() {
  local CORE_CNT=${AN_CORE_CNT:-100}
  local MEM_CNT=${AN_MEM_CNT:-1}

  cp -r $BASE/sim/payloads $WORKDIR/text
  cd text
  make CORE_CNT=$CORE_CNT
  cd ..

  local DATA_DIR=$WORKDIR/data
  local DRAM_CFG=${AN_DRAMSIM_CONF:-$BASE/sim/mem.cfg}

  mkdir -p $DATA_DIR
  if [[ $TESTCASE == "brunel" ]]; then
    $BASE/work/bin/datagen --core-cnt $CORE_CNT --tot-neuron 15000 --connectivity 0.05 --pre-simulate 95 --tau 0.01 --dump $DATA_DIR
  elif [[ $TESTCASE == "brette" ]]; then
    $BASE/work/bin/datagen --core-cnt $CORE_CNT --tot-neuron 10000 --connectivity 0.02 --pre-simulate 20 --inh-ratio 0.237 --dump $DATA_DIR
  elif [[ $TESTCASE == "vogels" ]]; then
    $BASE/work/bin/datagen --core-cnt $CORE_CNT --tot-neuron 40000 --connectivity 0.02 --pre-simulate 20 --inh-ratio 0.279 --dump $DATA_DIR
  elif [[ $TESTCASE == "potjans" ]]; then
    $BASE/work/bin/datagen --core-cnt $CORE_CNT --tot-neuron 8000 --connectivity 0.046875 --pre-simulate 20 --inh-ratio 0.255 --tau 0.001 --dump $DATA_DIR
  elif [[ $TESTCASE == "mvc" ]]; then
    $BASE/work/bin/datagen --core-cnt $CORE_CNT --load-nest-nodes $BASE/sim/mvc/nodes.json --load-nest-conns $BASE/sim/mvc/conns.json --nest-randomize --dump $DATA_DIR --pre-simulate 100
  elif [[ $TESTCASE == "sudoku" ]]; then
    $BASE/work/bin/datagen --core-cnt $CORE_CNT --tot-neuron 6561 --sudoku --dump $DATA_DIR
  elif [[ $TESTCASE == "mnist" ]]; then
    $BASE/work/bin/datagen --core-cnt $CORE_CNT --tot-neuron 794 --mnist 0.4 --pre-simulate 1 --dump $DATA_DIR
  else
    echo "Unsupported testcase"
    exit 1
  fi
  echo "======= Data generator: Ret value: $?"

  local TEXT_BIN
  if [[ "$MEM_CNT" == "1" ]]; then
    TEXT_BIN=$WORKDIR/text/test.single.bin
  elif [[ "$MEM_CNT" == "2" ]]; then
    TEXT_BIN=$WORKDIR/text/test.double.bin
  else
    echo "Invalid memory count: $MEM_CNT. Only supports 1 and 2 memories"
    exit 1
  fi

  # The double in test.double.bin means two memory channels
  local BASELINE_DRAM_LOG="$WORKDIR/baseline/dram"
  mkdir -p $BASELINE_DRAM_LOG
  $BASE/work/bin/sim_system $TEXT_BIN --hard --data $DATA_DIR --dram-config $DRAM_CFG --dram-log $BASELINE_DRAM_LOG | tee $WORKDIR/baseline/log.txt
  echo "======= Simulator (baseline): Ret value: $?"

  local ENHANCED_DRAM_LOG="$WORKDIR/enhanced/dram"
  mkdir -p $ENHANCED_DRAM_LOG
  $BASE/work/bin/sim_system $TEXT_BIN --soft --data $DATA_DIR --dram-config $DRAM_CFG --dram-log $ENHANCED_DRAM_LOG | tee $WORKDIR/enhanced/log.txt
  echo "======= Simulator (enhanced): Ret value: $?"

  rm -rf $DATA_DIR
}

work | tee $WORKDIR/log.full.txt
