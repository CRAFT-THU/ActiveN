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

AVG_BW=$(extract average_bandwidth)
echo -e "DRAM:\n  Average Bandwidth (GB/s): $AVG_BW"

TOT_CYC=$(extract num_cycles)
ACT_CYC=$(extract rank_active_cycles)
ACT_RATIO=$(python -c "print(100 * $ACT_CYC / $TOT_CYC)")
echo "  Active Cycles: $ACT_RATIO%"
