#!/usr/bin/env bash

set -e

BASE=$(readlink -f $(dirname $(readlink -f $0))/../..)
WORKDIR=$(readlink -f $1)
mkdir -p $WORKDIR

echo "Workdir: $1" >&1

cd $WORKDIR
# git lfs pull

function work() {
  export MEOW_CORE_CNT=${AN_CORE_CNT:-512}
  export MEOW_MEM_CNT=${AN_MEM_CNT:-2}

  cp -r $BASE/sim/payloads $WORKDIR/text
  cd text
  make CORE_CNT=$MEOW_CORE_CNT
  cd ..

  export MEOW_DATA=$WORKDIR/data
  export MEOW_MEM=$BASE/sim/mem.cfg
  if [[ "$MEOW_MEM_CNT" == "1" ]]; then
    export MEOW_TEXT=$WORKDIR/text/test.single.bin
    export MEOW_MEM_LOG="$WORKDIR/dram"
  elif [[ "$MEOW_MEM_CNT" == "2" ]]; then
    export MEOW_TEXT=$WORKDIR/text/test.double.bin
    export MEOW_MEM_LOG="$WORKDIR/dram"
  else
    echo "Invalid memory count: $MEOW_MEM_CNT. Only supports 1 and 2 memories"
    exit 1
  fi

  export MEOW_LOG=
  export MEOW_TRACE=

  mkdir -p $MEOW_DATA
  mkdir -p $MEOW_MEM_LOG

  $BASE/work/bin/datagen --core-cnt $MEOW_CORE_CNT --load-nest-nodes $BASE/sim/mvc/nodes.json --load-nest-conns $BASE/sim/mvc/conns.json --nest-randomize --dump $MEOW_DATA --pre-simulate 100
  echo "======= Data generator: Ret value: $?"
  $BASE/work/bin/sim.double
  echo "======= Simulator: Ret value: $?"
}

work | tee $WORKDIR/log.txt
