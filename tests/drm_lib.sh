#!/bin/bash

SOURCE_DIR="$( dirname "${BASH_SOURCE[0]}" )"
. $SOURCE_DIR/drm_getopt.sh

NAME=$(basename "$0")

dynamic_debug=

hda_dynamic_debug_enable() {
	if [ -e "$dynamic_debug" ]; then
		echo -n "module snd_hda_intel +pf" > $dynamic_debug
		echo -n "module snd_hda_core +pf" > $dynamic_debug
	fi
}

hda_dynamic_debug_disable() {
	if [ -e "$dynamic_debug" ]; then
		echo -n "module snd_hda_core =_" > $dynamic_debug
		echo -n "module snd_hda_intel =_" > $dynamic_debug
	fi
}

KERN_EMER="<0>"
KERN_ALERT="<1>"
KERN_CRIT="<2>"
KERN_ERR="<3>"
KERN_WARNING="<4>"
KERN_NOTICE="<5>"
KERN_INFO="<6>"
KERN_DEBUG="<7>"

kmsg() {
	echo "$@" > /dev/kmsg
}

finish() {
	exitcode=$?
	hda_dynamic_debug_disable
	kmsg "${KERN_INFO}[IGT] $NAME: exiting, ret=$exitcode"
	exit $exitcode
}
trap finish EXIT

kmsg "${KERN_INFO}[IGT] $NAME: executing"

skip() {
	echo "$@"
	exit $IGT_EXIT_SKIP
}

die() {
	echo "$@"
	exit $IGT_EXIT_FAILURE
}

do_or_die() {
	$@ > /dev/null 2>&1 || (echo "FAIL: $@ ($?)" && exit $IGT_EXIT_FAILURE)
}

if [ -d /sys/kernel/debug ]; then
	debugfs_path=/sys/kernel/debug
elif [ -d /debug ]; then
	debugfs_path=/debug
else
	skip "debugfs not found"
fi

dynamic_debug=$debugfs_path/dynamic_debug/control
if [ ! -e "$dynamic_debug" ]; then
	echo "WARNING: dynamic debug control not available"
fi

if [ ! -d $debugfs_path/dri ]; then
	skip "dri debugfs not found"
fi

i915_dfs_path=x
for minor in `seq 0 16`; do
	if [ -f $debugfs_path/dri/$minor/i915_error_state ] ; then
		i915_dfs_path=$debugfs_path/dri/$minor
		break
	fi
done

if [ $i915_dfs_path = "x" ] ; then
	skip " i915 debugfs path not found."
fi

# read everything we can
if [ `cat $i915_dfs_path/clients | wc -l` -gt "2" ] ; then
	[ -n "$DRM_LIB_ALLOW_NO_MASTER" ] || \
		die "ERROR: other drm clients running"
fi

whoami | grep -q root || ( echo ERROR: not running as root; exit $IGT_EXIT_FAILURE )

i915_sfs_path=
if [ -d /sys/class/drm ] ; then
    sysfs_path=/sys/class/drm
    if [ -f $sysfs_path/card$minor/error ] ; then
	    i915_sfs_path="$sysfs_path/card$minor"
    fi
fi
# sysfs may not exist as the 'error' is a new interface in 3.11

function drmtest_skip_on_simulation()
{
	[ -n "$INTEL_SIMULATION" ] && exit $IGT_EXIT_SKIP
}

drmtest_skip_on_simulation
