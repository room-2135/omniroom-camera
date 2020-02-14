#!/usr/bin/bash
./omniroom-camera --local-id `ip a s $2 | grep ether | awk '{$1=$1};1' | sed 's/://g' | cut -d ' ' -f 2` --input-stream "rpicamsrc bitrate=1000000 ! video/x-h264,width=1920,height=1080,framerate=30/1 ! h264parse" $@
