#!/system/bin/sh
#
# Checks for power hal and makes a backup
#
bb=/sbin/bbx;

# Interactive tunables;
echo 40000 1094000:30000 1190000:10000 > /sys/devices/system/cpu/cpufreq/interactive/above_hispeed_delay
echo 85 1094000:80 1190000:95 > /sys/devices/system/cpu/cpufreq/interactive/target_loads
echo 40000 > /sys/devices/system/cpu/cpufreq/interactive/timer_rate
echo 40000 1094000:20000 > /sys/devices/system/cpu/cpufreq/interactive/timer_slack

# Disable PoweHAL from the rom-side;
$bb mount -o rw,remount /system;
if $bb [ -e /system/lib/hw/power.msm8226.so ]; then
  $bb [ -e /system/lib/hw/power.msm8226.so.backup ] || $bb cp /system/lib/hw/power.msm8226.so /system/lib/hw/power.msm8226.so.backup;
  $bb [ -e /system/lib/hw/power.msm8226.so ] && $bb rm -f /system/lib/hw/power.msm8226.so;
fi;

$bb mount -o ro,remount /system;
