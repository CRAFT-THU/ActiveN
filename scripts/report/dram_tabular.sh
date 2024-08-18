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

AVG_BW=$(node -e "console.log(($(extract average_bandwidth)).toFixed(3))")
TOT_CYC=$(extract num_cycles)
ACT_CYC=$(extract rank_active_cycles)
ACT_RATIO=$(node -e "console.log((100 * $ACT_CYC / $TOT_CYC).toFixed(3))")
echo -en "$AVG_BW\t$ACT_RATIO%"
