#!/usr/bin/bash
./omniroom-camera --local-id `ip a s eno1 | grep ether | awk '{$1=$1};1' | cut -d ' ' -f 2`
