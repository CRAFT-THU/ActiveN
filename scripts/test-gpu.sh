#!/usr/bin/env bash

BASE=$(readlink -f $(dirname $(readlink -f $0))/..)

cd $BASE/extras/gpu-baseline
make
./mvc ./mvc.genn
