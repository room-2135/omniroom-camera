#!/usr/bin/bash
./omniroom-camera --local-id `ip a s enp2s0 | grep ether | awk '{$1=$1};1' | cut -d ' ' -f 2`
