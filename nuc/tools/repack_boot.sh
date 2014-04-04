#!/bin/bash
# Repacks kernel and ramdisk into a bootimg

# 1.repack ramdisk
./repack_ramdisk ../ramdisk/dlxj_kk_boot/ramdisk
cp ../ramdisk/dlxj_kk_boot/new-ramdisk.cpio.gz ../ramdisk/new-ramdisk.cpio.gz

# 2.repack zImage and modules


# 3.repack boot.img
./mkbootimg --kernel ../kernel_image/zImage --ramdisk ../ramdisk/new-ramdisk.cpio.gz --board Deluxe_J --base 0x80600000 --pagesize 2048 --ramdiskaddr 0x81e00000 --output ../dlxj_boot_repacked.img
