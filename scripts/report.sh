#!/usr/bin/env bash

BASE=$(readlink -f $(dirname $(readlink -f $0))/..)

cd $BASE

mkdir -p work/report

function report() {
  echo $1 | tee -a work/report/perf.txt
  echo "====" | tee -a work/report/perf.txt
  ./scripts/report/log.js $2 | tee -a work/report/perf.txt
  ./scripts/report/dram.sh $2 | tee -a work/report/perf.txt
  echo | tee -a work/report/perf.txt
}

# Report MVC test
report "MVC Performance Test" ./work/mvc
