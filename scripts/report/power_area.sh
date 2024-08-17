#!/usr/bin/env bash

WORKDIR=$1
DIR=$(dirname $(readlink -f $0))

function extract() {
  KW=$1
  RESULT=$(for LOG in $(find $WORKDIR/dram | grep dramsim3.txt); do
    grep $KW $LOG | awk '{ print $3 }'
  done | python $DIR/sum.py)
  echo "$RESULT"
}

DRAM_POWER=$(extract average_power)
echo "DRAM: Dynamic Power (mW): $DRAM_POWER"
