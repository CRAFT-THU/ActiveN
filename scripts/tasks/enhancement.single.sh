#!/usr/bin/env bash

set -e

BASE=$(readlink -f $(dirname $(readlink -f $0))/../..)
WORKDIR=$(readlink -f $1)/$2
mkdir -p $WORKDIR

echo "Workdir: $1/$2" >&1

cd $WORKDIR
# git lfs pull

export MEOW_TESTCASE=$2

function work() {
  export MEOW_CORE_CNT=${AN_CORE_CNT:-100}
  export MEOW_MEM_CNT=${AN_MEM_CNT:-1}

  cp -r $BASE/sim/payloads $WORKDIR/text
  cd text
  make CORE_CNT=$MEOW_CORE_CNT
  cd ..

  export MEOW_DATA=$WORKDIR/data
  export MEOW_MEM=${AN_DRAMSIM_CONF:-$BASE/sim/mem.cfg}

  export MEOW_LOG=
  export MEOW_TRACE=

  mkdir -p $MEOW_DATA
  if [[ $MEOW_TESTCASE == "brunel" ]]; then
    $BASE/work/bin/datagen --core-cnt $MEOW_CORE_CNT --tot-neuron 15000 --connectivity 0.05 --pre-simulate 95 --tau 0.01 --dump $MEOW_DATA
  elif [[ $MEOW_TESTCASE == "brette" ]]; then
    $BASE/work/bin/datagen --core-cnt $MEOW_CORE_CNT --tot-neuron 10000 --connectivity 0.02 --pre-simulate 20 --inh-ratio 0.237 --dump $MEOW_DATA
  elif [[ $MEOW_TESTCASE == "vogels" ]]; then
    $BASE/work/bin/datagen --core-cnt $MEOW_CORE_CNT --tot-neuron 40000 --connectivity 0.02 --pre-simulate 20 --inh-ratio 0.279 --dump $MEOW_DATA
  elif [[ $MEOW_TESTCASE == "potjans" ]]; then
    $BASE/work/bin/datagen --core-cnt $MEOW_CORE_CNT --tot-neuron 8000 --connectivity 0.046875 --pre-simulate 20 --inh-ratio 0.255 --tau 0.001 --dump $MEOW_DATA
  elif [[ $MEOW_TESTCASE == "mvc" ]]; then
    $BASE/work/bin/datagen --core-cnt $MEOW_CORE_CNT --load-nest-nodes $BASE/sim/mvc/nodes.json --load-nest-conns $BASE/sim/mvc/conns.json --nest-randomize --dump $MEOW_DATA --pre-simulate 100
  elif [[ $MEOW_TESTCASE == "sudoku" ]]; then
    $BASE/work/bin/datagen --core-cnt $MEOW_CORE_CNT --tot-neuron 6561 --sudoku --dump $MEOW_DATA
  elif [[ $MEOW_TESTCASE == "mnist" ]]; then
    $BASE/work/bin/datagen --core-cnt $MEOW_CORE_CNT --tot-neuron 794 --mnist 0.4 --pre-simulate 1 --dump $MEOW_DATA
  else
    echo "Unsupported testcase"
    exit 1
  fi
  echo "======= Data generator: Ret value: $?"

  if [[ "$MEOW_MEM_CNT" == "1" ]]; then
    export MEOW_TEXT=$WORKDIR/text/test.single.bin
  elif [[ "$MEOW_MEM_CNT" == "2" ]]; then
    export MEOW_TEXT=$WORKDIR/text/test.double.bin
  else
    echo "Invalid memory count: $MEOW_MEM_CNT. Only supports 1 and 2 memories"
    exit 1
  fi

  # The double in test.double.bin means two memory channels
  export MEOW_MEM_LOG="$WORKDIR/baseline/dram"
  mkdir -p $MEOW_MEM_LOG
  $BASE/work/bin/sim.single | tee $WORKDIR/baseline/log.txt
  echo "======= Simulator (baseline): Ret value: $?"

  export MEOW_MEM_LOG="$WORKDIR/enhanced/dram"
  mkdir -p $MEOW_MEM_LOG
  $BASE/work/bin/sim.double | tee $WORKDIR/enhanced/log.txt
  echo "======= Simulator (enhanced): Ret value: $?"

  rm -rf $MEOW_DATA
}

work | tee $WORKDIR/log.full.txt
