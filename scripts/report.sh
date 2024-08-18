#!/usr/bin/env bash

BASE=$(readlink -f $(dirname $(readlink -f $0))/..)

cd $BASE

mkdir -p work/report

# Arguments
# 1. Title
# 2. Working directory
# 3. Prefix for generated reports
function report() {
  echo -e "Performance\n---"
  ./scripts/report/log.js $1 100 | tee -a work/report/perf.txt
  ./scripts/report/dram.sh $1 | tee -a work/report/perf.txt
  echo >> work/report/perf.txt

  echo -e "Power & Area\n----"
  ./scripts/report/power_area.sh $1 | tee -a work/report/power_area.txt
  echo | tee -a work/report/power_area.txt
}

function report_tabular() {
  echo -n $3
  ./scripts/report/dram_tabular.sh $1
  echo -ne "\t| "
  ./scripts/report/log.js $1 100 simple
}

echo -n > work/report/perf.txt
echo -n > work/report/power_area.txt

# Report MVC test
echo -e "====================\nMVC Performance Test\n====================" | tee -a work/report/perf.txt
report ./work/mvc

# Report scalability test
echo -e "====================\nScalability Test\n====================" | tee -a work/report/perf.txt

echo -e "HBM2x2\n----" | tee -a work/report/perf.txt
echo -e "Cores\tDRAM BW\tUtil\t| Time    \tPU Occu\tFU Util" | tee -a work/report/perf.txt
echo -e "     \t(GB/s) \t(%) \t| (ms)    \t(%)    \t(%)" | tee -a work/report/perf.txt
for task in $(seq 128 16 512); do
  echo -ne "$task\t" | tee -a work/report/perf.txt
  report_tabular ./work/scalability/$task/double work/report | tee -a work/report/perf.txt
done

echo -e "\nHBM2x1\n----" | tee -a work/report/perf.txt
echo -e "Cores\tDRAM BW\tUtil\t| Time    \tPU Occu\tFU Util" | tee -a work/report/perf.txt
echo -e "     \t(GB/s) \t(%) \t| (ms)    \t(%)    \t(%)" | tee -a work/report/perf.txt
for task in $(seq 128 16 512); do
  echo -ne "$task\t" | tee -a work/report/perf.txt
  report_tabular ./work/scalability/$task/single work/report | tee -a work/report/perf.txt
done

echo "" | tee -a work/report/perf.txt

# Report enhanced core tests
echo -e "====================\nEnhanced Core Test\n====================" | tee -a work/report/perf.txt
echo -e "Task\tCycles\tNorPerf\tBL Util\tEnhanced Util" | tee -a work/report/perf.txt
echo -e "    \t(/r)  \t       \t(%)    \t(%)" | tee -a work/report/perf.txt
for task in $(cat scripts/tasks/enhancement.list); do
  echo -ne "$task\t" | tee -a work/report/perf.txt
  ./scripts/report/enhancement.sh work/enhancement/$task | tee -a work/report/perf.txt
done
echo ---- | tee -a work/report/perf.txt
cat scripts/report/enhancement.note | tee -a work/report/perf.txt
echo | tee -a work/report/perf.txt
