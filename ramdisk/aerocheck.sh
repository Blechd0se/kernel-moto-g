#!/sbin/bbx sh
#
# Checks for power hal and makes a backup
#

# Interactive tunables;
echo 30000 1094000:40000 1190000:20000 > /sys/devices/system/cpu/cpufreq/interactive/above_hispeed_delay
echo 85 1094000:80 1190000:95 > /sys/devices/system/cpu/cpufreq/interactive/target_loads
echo 30000 1094000:20000 1190000:10000 > /sys/devices/system/cpu/cpufreq/interactive/timer_rate
echo 50000 1094000:30000 > /sys/devices/system/cpu/cpufreq/interactive/timer_slack

# Disable PoweHAL from the rom-side;
/sbin/bbx mount -o rw,remount /system;
if /sbin/bbx [ -e /system/lib/hw/power.msm8226.so ]; then
  /sbin/bbx [ -e /system/lib/hw/power.msm8226.so.backup ] || /sbin/bbx cp /system/lib/hw/power.msm8226.so /system/lib/hw/power.msm8226.so.backup;
  /sbin/bbx [ -e /system/lib/hw/power.msm8226.so ] && /sbin/bbx rm -f /system/lib/hw/power.msm8226.so;
fi;

# Enable init.d with permissions;
if /sbin/bbx [ ! -e /system/etc/init.d ]; then
  /sbin/bbx mkdir /system/etc/init.d;
  /sbin/bbx chown -R root.root /system/etc/init.d;
  /sbin/bbx chmod -R 755 /system/etc/init.d;
fi;
/sbin/bbx mount -o ro,remount /system;
