#!/bin/sh

log()
{
    printf '%s\n' "app_init: $*" >&2
}

mount_microsd()
{
    sd_device=/dev/mmcblk1p1
    wait_count=0

    if grep -qs '[[:space:]]/mnt[[:space:]]' /proc/mounts; then
        log "/mnt is already mounted"
        return 0
    fi

    # Device discovery normally completes before S90autorun launches us, but
    # allow a short bounded delay for a card that enumerates slowly.
    while [ ! -b "$sd_device" ] && [ "$wait_count" -lt 5 ]; do
        sleep 1
        wait_count=$((wait_count + 1))
    done

    if [ ! -b "$sd_device" ]; then
        log "no microSD partition at $sd_device; continuing without /mnt"
        return 1
    fi
    if [ ! -d /mnt ]; then
        log "mountpoint /mnt is missing; continuing without microSD"
        return 1
    fi

    mount_options=rw,noatime,nodiratime,umask=077,errors=remount-ro
    if mount -t exfat -o "$mount_options" "$sd_device" /mnt 2>/dev/null ||
       mount -t vfat -o "$mount_options" "$sd_device" /mnt 2>/dev/null; then
        log "mounted $sd_device at /mnt"
        return 0
    fi

    log "could not mount $sd_device as exFAT or FAT; continuing without /mnt"
    return 1
}

prepare_media_storage()
{
    mount_microsd || return 1
    # A newly formatted card has no vendor-created DCIM directory. The media
    # writers create their own dated folders, but need these parents first.
    # Only create them on mounted storage, never on the underlying rootfs.
    if ! mkdir -p /mnt/DCIM/capture /mnt/DCIM/record; then
        log "could not prepare photo and recording directories on microSD"
        return 1
    fi
}

# UART3 RX/TX: internal SIYI link to the gimbal controller.
bspmm 0x0102f012c 0x1201
bspmm 0x0102f0130 0x1201

# UART4 RX/TX: retain the known-good vendor pinmux state.
bspmm 0x0102f0134 0x1201
bspmm 0x0102f0138 0x1201

export LD_LIBRARY_PATH="/app/libs:${LD_LIBRARY_PATH:-}"
export PATH="/app/bin:${PATH:-/usr/bin:/usr/sbin:/bin:/sbin}"
export TMP=/run

/sbin/ifconfig eth0:1 192.168.1.25 up

# Keep UniGCS discovery reachable on retained vendor Ethernet drivers, which
# can drop multicast despite a successful IP_ADD_MEMBERSHIP (observed on A8).
/sbin/ifconfig eth0 allmulti || log "warning: could not enable Ethernet multicast reception"

# Firmware overlays carry only an initial camera-app configuration. Preserve
# parameters already selected through the web UI across subsequent upgrades.
if [ ! -e /app/camera.ini ] && [ -r /app/camera.ini.default ]; then
    umask 022
    if cp /app/camera.ini.default /app/camera.ini.new &&
       chmod 0644 /app/camera.ini.new &&
       /bin/fsync /app/camera.ini.new &&
       mv /app/camera.ini.new /app/camera.ini; then
        /bin/fsync /app || true
    else
        rm -f /app/camera.ini.new
        log "warning: could not initialize /app/camera.ini"
    fi
fi

# Firmware overlays carry only an initial default. Never replace an admin
# password already chosen through the Users page.
if [ ! -s /app/web.pass ] && [ -r /app/web.pass.default ]; then
    umask 077
    if cp /app/web.pass.default /app/web.pass.new &&
       chmod 0600 /app/web.pass.new &&
       /bin/fsync /app/web.pass.new &&
       mv /app/web.pass.new /app/web.pass; then
        /bin/fsync /app || true
    else
        rm -f /app/web.pass.new
        log "warning: could not initialize /app/web.pass"
    fi
fi

# Keep the clock synchronized from Phoenix on the isolated camera network.
if [ -x /app/bin/mt11-timesync.sh ]; then
    /app/bin/mt11-timesync.sh >/run/mt11-timesync.log 2>&1 &
fi

# Mount removable storage before either camera application or product_upgrade
# can inspect /mnt. WARNING: this also exposes any /mnt/MT11_FW_* package to
# the automatic vendor updater. Failure is non-fatal so a bad/absent card
# cannot suppress camera and network service startup.
prepare_media_storage || true

# Retain the vendor auxiliary HTTP server.
if [ -x /app/boa/boa ]; then
    /app/boa/boa &
fi

# Local administrative web service.
if [ -x /app/bin/mt11-web ]; then
    /app/bin/mt11-web -p 80 >/run/mt11-web.log 2>&1 &
else
    log "warning: /app/bin/mt11-web is unavailable"
fi

# Start the support SSH service before handing hardware to a camera app.
# Dropbear checks getrandom() before daemonizing. Run its launcher in the
# background so an entropy-starved early boot cannot prevent camera startup;
# camera hardware activity then lets the kernel CRNG initialize and Dropbear
# continue safely.
if [ -x /app/dropbear/start-dropbear.sh ]; then
    /app/dropbear/start-dropbear.sh >/run/dropbear.log 2>&1 &
else
    log "warning: /app/dropbear/start-dropbear.sh is unavailable"
fi

# Start AP CameraGimbal with bounded log capture. The historical script name
# remains compatible with existing installations.
if [ -x /app/app_selection.sh ]; then
    /app/app_selection.sh &
else
    log "error: /app/app_selection.sh is unavailable or not executable"
fi
