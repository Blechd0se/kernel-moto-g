#!/system/bin/sh

PATH=/sbin:/system/sbin:/system/bin:/system/xbin
export PATH

while getopts d op;
do
	case $op in
		d)  dbg_on=1;;
	esac
done
shift $(($OPTIND-1))

debug()
{
	[ $dbg_on ] && echo "Debug: $*"
}

error_and_leave()
{
	err_code=$1
	case $err_code in
		1)  echo "Error: No response from touch IC";;
		2)  echo "Error: Cannot read property $1";;
		3)  echo "Error: No matching firmware file found";;
		4)  echo "Error: Touch IC is in bootloader mode";;
		5)  echo "Error: Touch provides no reflash interface";;
		6)  echo "Error: Touch driver is not running";;
	esac
	exit $err_code
}

wait_ic_poweron()
{
	debug "wait until driver reports <IC power is on>..."
	while true; do
		readiness=$(cat $touch_path/poweron)
		if [ "$readiness" == "1" ]; then
			debug "power is on!!!"
			break;
		fi
		sleep 1
		debug "not ready; keep waiting..."
	done
	unset readiness
}

read_touch_property()
{
	property=""
	debug "retrieving property: [$touch_path/$1]"
	property=$(cat $touch_path/$1 2> /dev/null)
	debug "touch property [$1] is: [$property]"
	[ -z "$property" ] && return 1
	return 0
}

find_latest_build_id()
{
	debug "scanning dir for files matching [$1]"
	str_build_id_latest=""
	let dec=0; max=0;
	for file in $(ls $1 2>/dev/null);
	do
		x=${file#*-}; y=${x#*-}; z=${y#*-}; str_hex=${z%%-*};
		let dec=0x$str_hex
		if [ $dec -gt $max ];
		then
			let max=$dec; dec_build_id_latest=$dec;
			str_build_id_latest=$str_hex
		fi
	done
	unset dec max x z str_hex
	[ -z "$str_build_id_latest" ] && return 1
	return 0
}


find_latest_config_id()
{
	debug "scanning dir for files matching [$1]"
	str_cfg_id_latest=""
	let dec=0; max=0;
	for file in $(ls $1 2>/dev/null);
	do
		x=${file#*-}; z=${x#*-}; str_hex=${z%%-*};
		let dec=0x$str_hex
		if [ $dec -gt $max ];
		then
			let max=$dec; dec_cfg_id_latest=$dec;
			str_cfg_id_latest=$str_hex
		fi
	done
	unset dec max x z str_hex
	[ -z "$str_cfg_id_latest" ] && return 1
	return 0
}

flash_fw()
{
	fw_file=$1
	fw_file_build_id=$2
	fw_file_cfg_id=$3
	debug "flashing fw file [$fw_file]"

	# must be awake before flash start
	wait_ic_poweron

	debug "forcing firmware upgrade"
	echo 1 > $touch_path/forcereflash
	debug "sending reflash command"
	echo $fw_file > $touch_path/doreflash

	# must be awake before flashprog can be read
	wait_ic_poweron
	read_touch_property flashprog || error_and_leave 1
	bl_mode=$property
	if [ "$bl_mode" == "1" ]; then
		echo "In bootloader mode"
	fi

	read_touch_property buildid || error_and_leave 1
	str_build_id_new=${property%-*}
	str_cfg_id_new=${property#*-}

	debug "firmware build ids: expected $str_build_id_latest, current $str_build_id_new"
	debug "firmware config ids: expected $str_cfg_id_latest, current $str_cfg_id_new"

	echo "Touch firmware build id at start $str_build_id_start"
	echo "Touch firmware build id in the file $fw_file_build_id"
	echo "Touch firmware build id currently programmed $str_build_id_new"
	echo "Touch firmware config id at start $str_cfg_id_start"
	echo "Touch firmware config id in the file $fw_file_cfg_id"
	echo "Touch firmware config id currently programmed $str_cfg_id_new"

	# current id values become start for next procedure if any
	str_build_id_start=$str_build_id_new
	let dec_build_id_start=0x$str_build_id_start
	str_cfg_id_start=$str_cfg_id_new
	let dec_cfg_id_start=0x$str_cfg_id_start

	unset fw_file
	unset fw_file_build_id
	unset fw_file_cfg_id
	unset bl_mode
	unset str_build_id_new
	unset str_cfg_id_new
}

for touch_vendor in $*; do
	debug "searching driver for vendor [$touch_vendor]"
	touch_driver_link=$(ls -l /sys/bus/i2c/drivers/$touch_vendor*/*-*)
	if [ -z "$touch_driver_link" ]; then
		debug "no driver for vendor [$touch_vendor] is running"
		shift 1
	else
		debug "driver for vendor [$touch_vendor] found!!!"
		break
	fi
done

[ -z "$touch_driver_link" ] && error_and_leave 6

touch_path=/sys/devices/${touch_driver_link#*devices/}
debug "sysfs touch path: $touch_path"

[ -f $touch_path/doreflash ] || error_and_leave 5
[ -f $touch_path/poweron ] || error_and_leave 5

device_property=ro.hw.device
hwrev_property=ro.hw.revision
firmware_path=/system/etc/firmware

let dec_cfg_id_start=0; dec_cfg_id_latest=0;
let dec_build_id_start=0; dec_build_id_latest=0;
let dec_blu_build_id_latest=0;

# must be awake before flashprog can be read
wait_ic_poweron
read_touch_property flashprog || error_and_leave 1
bl_mode=$property
debug "bl mode: $bl_mode"

read_touch_property productinfo || error_and_leave 1
touch_product_id=$property
if [ -z "$touch_product_id" ] || [ "$touch_product_id" == "0" ];
then
	debug "touch ic reports invalid product id"
	error_and_leave 3
fi
debug "touch product id: $touch_product_id"

read_touch_property buildid || error_and_leave 1

str_build_id_start=${property%-*}
let dec_build_id_start=0x$str_build_id_start
debug "touch build id: $str_build_id_start"

str_cfg_id_start=${property#*-}
let dec_cfg_id_start=0x$str_cfg_id_start
debug "touch config id: $str_cfg_id_start"

product_id=$(getprop $device_property 2> /dev/null)
[ -z "$product_id" ] && error_and_leave 2 $device_property
product_id=${product_id%-*}
debug "product id: $product_id"

hwrev_id=$(getprop $hwrev_property 2> /dev/null)
[ -z "$hwrev_id" ] && error_and_leave 2 $hwrev_property
debug "hw revision: $hwrev_id"

cd $firmware_path

debug "search for bootloader update with best hw revision match"
blu_prefix="BLU_"
blu_cfg_id="00000000"
hw_mask="-$hwrev_id"
while [ ! -z "$hw_mask" ]; do
	if [ "$hw_mask" == "-" ]; then
		hw_mask=""
	fi
	find_latest_build_id "$blu_prefix$touch_vendor-$touch_product_id-$blu_cfg_id-*-$product_id$hw_mask.*"
	if [ $? -eq 0 ]; then
		debug "found BLU"
		break;
	fi
        hw_mask=${hw_mask%?}
done

blu_file=""
dec_blu_build_id_latest=""
if [ ! -z "$str_build_id_latest" ]; then
	blu_file=$(ls $blu_prefix$touch_vendor-$touch_product_id-$blu_cfg_id-$str_build_id_latest-$product_id$hw_mask.*)
	let dec_blu_build_id_latest=$dec_build_id_latest
	debug "touch bootloader update file for upgrade $blu_file"
fi

debug "search for firmware with best hw revision match"
hw_mask="-$hwrev_id"
while [ ! -z "$hw_mask" ]; do
	if [ "$hw_mask" == "-" ]; then
		hw_mask=""
	fi
	find_latest_config_id "$touch_vendor-$touch_product_id-*-$product_id$hw_mask.*"
	if [ $? -eq 0 ]; then
		break;
	fi
        hw_mask=${hw_mask%?}
done

[ -z "$str_cfg_id_latest" ] && error_and_leave 3

firmware_file=$(ls $touch_vendor-$touch_product_id-$str_cfg_id_latest-*-$product_id$hw_mask.*)
debug "firmware file for upgrade $firmware_file"

if [ -z $blu_file ]; then
	debug "Touch firmware bootloader update not found"
fi

if [ ! -z $blu_file ] && [ $dec_build_id_start -lt $dec_blu_build_id_latest ]; then
	flash_fw $blu_file $str_blu_build_id_latest $blu_cfg_id
else
	echo "Touch firmware bootloader is up to date"
fi

if [ $dec_cfg_id_start -ne $dec_cfg_id_latest ] || [ "$bl_mode" == "1" ];
then
	flash_fw $firmware_file $str_build_id_latest $str_cfg_id_latest

	# must be awake before flashprog can be read
	wait_ic_poweron
	read_touch_property flashprog || error_and_leave 1
	bl_mode=$property
	[ "$bl_mode" == "1" ] && error_and_leave 4
else
	echo "Touch firmware is up to date"
fi

unset device_property hwrev_property
unset str_cfg_id_start str_cfg_id_latest str_cfg_id_new
unset dec_cfg_id_start dec_cfg_id_latest
unset hwrev_id product_id touch_product_id
unset synaptics_link firmware_path touch_path
unset bl_mode dbg_on hw_mask firmware_file property
unset dec_build_id_start dec_build_id_latest dec_blu_build_id_latest
unset str_build_id_start str_build_id_latest blu_prefix blu_cfg_id
unset blu_file blu_build_id

return 0
