#!/usr/bin/bash
./omniroom-camera --local-id `ip a s $1 | grep ether | awk '{$1=$1};1' | sed 's/://g' | cut -d ' ' -f 2` $@
