#!/usr/bin/env bash

BASE=$(readlink -f $(dirname $(readlink -f $0))/..)

cd $BASE

mkdir -p work

# Run MVC test
./scripts/tasks/mvc.sh $BASE/work/mvc
