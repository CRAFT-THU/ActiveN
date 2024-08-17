#!/usr/bin/env bash

BASE=$(readlink -f $(dirname $(readlink -f $0))/..)

cd $BASE

mkdir -p work/report

# Arguments
# 1. Title
# 2. Working directory
# 3. Prefix for generated reports
function report() {
  echo $1 | tee -a $3/perf.txt
  echo "====" | tee -a $3/perf.txt
  echo "[Performance]"
  ./scripts/report/log.js $2 100 | tee -a $3/perf.txt
  ./scripts/report/dram.sh $2 | tee -a $3/perf.txt
  echo >> $3/perf.txt

  echo "[Power & Area]"
  echo $1 >> $3/power_area.txt.txt
  echo "====" >> $3/power_area.txt.txt
  ./scripts/report/power_area.sh $2 | tee -a $3/power_area.txt
  echo | tee -a $3/power_area.txt
}

echo -n > work/report/perf.txt
echo -n > work/report/power_area.txt

# Report MVC test
report "MVC Performance Test" ./work/mvc work/report

# Report scalability test
mkdir -p work/scalability/raw-report/single
mkdir -p work/scalability/raw-report/double
for task in $seq(128 512 16); do
  report "Raw data for scalability test, single mem, core count $task" ./work/scalability/$task/single work/scalability/raw/single
  report "Raw data for scalability test, double mem, core count $task" ./work/scalability/$task/double work/scalability/raw/double
done
