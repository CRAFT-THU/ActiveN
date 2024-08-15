#!/usr/bin/env bash

set -e

BASE=$(readlink -f $(dirname $(readlink -f $0))/../..)
WORKDIR=$(readlink -f $1)
mkdir -p $WORKDIR

echo "Workdir: $1" >&1

cd $WORKDIR
git lfs pull

function work() {
  export MEOW_CORE_CNT=512

  cp -r $BASE/sim/payloads $WORKDIR/text
  cd text
  make CORE_CNT=MEOW_CORE_CNT
  cd ..

  export MEOW_DATA=$WORKDIR/data
  export MEOW_TEXT=$WORKDIR/text/test.double.bin
  export MEOW_MEM=$BASE/sim/mem.cfg
  export MEOW_MEM_LOG="$WORKDIR/dram"
  export MEOW_MEM_CNT=2

  export MEOW_LOG=
  export MEOW_TRACE=

  mkdir -p $MEOW_DATA
  mkdir -p $MEOW_MEM_LOG

  $BASE/work/bin/datagen --core-cnt 512 --load-nest-nodes $BASE/sim/mvc/nodes.json --load-nest-conns $BASE/sim/mvc/conns.json --nest-randomize --dump $MEOW_DATA --pre-simulate 10
  echo "======= Data generator: Ret value: $?"
  $BASE/work/bin/sim.double
  echo "======= Simulator: Ret value: $?"
}

work | tee $WORKDIR/log.txt
