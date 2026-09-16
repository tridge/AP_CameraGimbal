#!/bin/sh
# Boot entry point launched by the stock /etc/init.d/rcS. The vendor updater
# replaces all of /opt/bin/gcu, so the overlay ships its own copy.
cd /opt/bin/gcu/ipc
./camera_gcu.sh &
cd
