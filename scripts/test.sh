#!/usr/bin/env bash

BASE=$(readlink -f $(dirname $(readlink -f $0))/..)

cd $BASE

mkdir -p work

# Run MVC test
./scripts/tasks/mvc.sh $BASE/work/mvc

# Run scalability test
./scripts/tasks/scalability.sh $BASE/work/scalability

# Run enhanced core test
./scripts/tasks/enhancement.sh $BASE/work/enhancement
