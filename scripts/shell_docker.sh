#!/usr/bin/env bash
BASE=$(readlink -f $(dirname $(readlink -f $0))/..)
docker build -t activen_nix .
docker run -it -v $BASE:/activen activen_nix
