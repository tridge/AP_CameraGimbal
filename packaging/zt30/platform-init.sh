#!/bin/sh
# Module setup from the SIYI ZT30 v0.2.9 distribution demo.sh.
# Retained /config owns the SDK, sensor tuning and kernel modules.
echo 4 > /proc/sys/kernel/printk

echo 5 > /proc/sys/vm/dirty_background_ratio
echo 10 > /proc/sys/vm/dirty_ratio
echo 100 > /proc/sys/vm/dirty_writeback_centisecs
echo 300 > /proc/sys/vm/dirty_expire_centisecs
insmod /config/modules/4.9.227/usb-common.ko
insmod /config/modules/4.9.227/usbcore.ko
insmod /config/modules/4.9.227/ehci-hcd.ko
insmod /config/modules/4.9.227/scsi_mod.ko
insmod /config/modules/4.9.227/usb-storage.ko
insmod /config/modules/4.9.227/usbhid.ko
insmod /config/modules/4.9.227/cifs.ko
insmod /config/modules/4.9.227/nls_utf8.ko
insmod /config/modules/4.9.227/grace.ko
insmod /config/modules/4.9.227/sunrpc.ko
insmod /config/modules/4.9.227/lockd.ko
insmod /config/modules/4.9.227/nfs.ko
insmod /config/modules/4.9.227/nfsv2.ko
insmod /config/modules/4.9.227/nfsv3.ko
insmod /config/modules/4.9.227/mmc_core.ko
insmod /config/modules/4.9.227/mmc_block.ko
insmod /config/modules/4.9.227/kdrv_sdmmc.ko
insmod /config/modules/4.9.227/fat.ko
insmod /config/modules/4.9.227/msdos.ko
insmod /config/modules/4.9.227/vfat.ko
insmod /config/modules/4.9.227/ntfs.ko
insmod /config/modules/4.9.227/sd_mod.ko
insmod /config/modules/4.9.227/ms_notify.ko
insmod /config/modules/4.9.227/mdrv-sata-host.ko
insmod /config/modules/4.9.227/cfg80211.ko
#kernel_mod_list
insmod /config/modules/4.9.227/mhal.ko
insmod /config/modules/4.9.227/exfat.ko
#misc_mod_list
insmod /config/modules/4.9.227/mi_common.ko
insmod /config/modules/4.9.227/mi_sys.ko cmdQBufSize=3072 logBufSize=256
insmod /config/modules/4.9.227/mi_sensor.ko
insmod /config/modules/4.9.227/mi_mipitx.ko
insmod /config/modules/4.9.227/mi_gfx.ko
insmod /config/modules/4.9.227/mi_rgn.ko
insmod /config/modules/4.9.227/mi_scl.ko
insmod /config/modules/4.9.227/mi_vdec.ko
insmod /config/modules/4.9.227/mi_hdmi.ko
insmod /config/modules/4.9.227/mi_isp.ko
insmod /config/modules/4.9.227/mi_ipu.ko
insmod /config/modules/4.9.227/mi_shadow.ko
insmod /config/modules/4.9.227/mi_jpd.ko
insmod /config/modules/4.9.227/mi_ao.ko
insmod /config/modules/4.9.227/mi_ldc.ko
insmod /config/modules/4.9.227/mi_ai.ko
insmod /config/modules/4.9.227/mi_panel.ko
insmod /config/modules/4.9.227/mi_disp.ko
insmod /config/modules/4.9.227/mi_vif.ko
insmod /config/modules/4.9.227/mi_venc.ko
insmod /config/modules/4.9.227/mi_vdisp.ko vdisp_bufq_depth=4
#mi module
major=`cat /proc/devices | busybox awk "\\$2==\""mi_poll"\" {print \\$1}"`
busybox mknod /dev/mi_poll c $major 0


#misc_mod_list_late
insmod /config/modules/4.9.227/imx415_MIPI.ko chmap=1 lane_num=4
insmod /config/modules/4.9.227/imx335_MIPI.ko chmap=2 lane_num=4
mdev -s

exec /customer/camera-app/app-init.sh
