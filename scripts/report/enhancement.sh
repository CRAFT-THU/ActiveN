#!/usr/bin/env bash

BASE=$(readlink -f $(dirname $(readlink -f $0))/../..)

cd $BASE

CASE=$1

BASELINE_CYCLES=$(grep "Core Cycles" $1/baseline/log.txt | grep -oE "[0-9]+$")
ENHANCED_CYCLES=$(grep "Core Cycles" $1/enhanced/log.txt | grep -oE "[0-9]+$")

BASELINE_UTIL=$(grep "Avg working ratio" $1/baseline/log.txt | tail -n1 | grep -oE "[.0-9]+$")
ENHANCED_UTIL=$(grep "Avg working ratio" $1/enhanced/log.txt | tail -n1 | grep -oE "[.0-9]+$")

format() {
  node -e "console.log(($1).toFixed(3))"
}

echo -e "$(format $ENHANCED_CYCLES/1000)k\t$(format 2-$ENHANCED_CYCLES/$BASELINE_CYCLES)\t$(format $BASELINE_UTIL*100)%\t$(format $ENHANCED_UTIL*100)%"
