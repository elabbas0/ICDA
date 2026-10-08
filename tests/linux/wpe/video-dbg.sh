#!/bin/sh
B=/volumes/fat32-3/linux/bin/busybox
/bin/busybox sh /volumes/fat32-3/video.sh &
$B sleep 420; $B cat /dev/lxdump; $B sleep 120; $B cat /dev/lxdump
