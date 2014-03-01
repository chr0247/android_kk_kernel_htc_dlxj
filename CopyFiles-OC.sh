#!/bin/sh
find . -iname "*.ko" -type f -exec cp {} /home/schqiushui/adb/modules/4.4/GPU_OC \;
cp ./arch/arm/boot/zImage /home/schqiushui/adb/modules/4.4/GPU_OC


