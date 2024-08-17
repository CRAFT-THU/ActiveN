#!/usr/bin/env bash

set -e

BASE=$(readlink -f $(dirname $(readlink -f $0))/../..)
cd $BASE

open_sem(){
  mkfifo pipe-$$
  exec 3<>pipe-$$
  rm pipe-$$
  local i=$1
  for((;i>0;i--)); do
    printf %s 000 >&3
  done
}

# run the given command asynchronously and pop/push tokens
run_with_lock(){
  local x
  # this read waits until there is something to read
  read -u 3 -n 3 x && ((0==x)) || exit $x
  (
   ( "$@"; )
  # push the return code of the command to the semaphore
  printf '%.3d' $? >&3
  )&
}

MEOW_PARALLELISM=${AN_PARALLEL_JOBS:-1}
echo "Parallelism: $MEOW_PARALLELISM"

open_sem $MEOW_PARALLELISM
# Run scalability with core counts ranging from 128 to 512, with step size 16
for task in $(seq 128 16 512); do
  echo $task
  run_with_lock ./scripts/tasks/scalability.single.sh $1 $task
done

wait
