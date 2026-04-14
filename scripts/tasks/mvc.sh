#!/usr/bin/env bash

set -e

BASE=$(readlink -f $(dirname $(readlink -f $0))/../..)
WORKDIR=$(readlink -f $1)
mkdir -p $WORKDIR

echo "Workdir: $1" >&1

cd $WORKDIR
# git lfs pull

function work() {
  local CORE_CNT=${AN_CORE_CNT:-512}
  local MEM_CNT=${AN_MEM_CNT:-2}

  cp -r $BASE/sim/payloads $WORKDIR/text
  cd text
  make CORE_CNT=$CORE_CNT
  cd ..

  local DATA_DIR=$WORKDIR/data
  local DRAM_CFG=$BASE/sim/mem.cfg
  local DRAM_LOG="$WORKDIR/dram"
  local TEXT_BIN
  if [[ "$MEM_CNT" == "1" ]]; then
    TEXT_BIN=$WORKDIR/text/test.single.bin
  elif [[ "$MEM_CNT" == "2" ]]; then
    TEXT_BIN=$WORKDIR/text/test.double.bin
  else
    echo "Invalid memory count: $MEM_CNT. Only supports 1 and 2 memories"
    exit 1
  fi

  mkdir -p $DATA_DIR
  mkdir -p $DRAM_LOG

  $BASE/work/bin/datagen --core-cnt $CORE_CNT --load-nest-nodes $BASE/sim/mvc/nodes.json --load-nest-conns $BASE/sim/mvc/conns.json --nest-randomize --dump $DATA_DIR --pre-simulate 100
  echo "======= Data generator: Ret value: $?"
  $BASE/work/bin/sim_system $TEXT_BIN --soft --data $DATA_DIR --dram-config $DRAM_CFG --dram-log $DRAM_LOG
  echo "======= Simulator: Ret value: $?"

  rm -rf $DATA_DIR
}

work | tee $WORKDIR/log.txt
