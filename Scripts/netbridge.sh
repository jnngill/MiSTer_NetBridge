#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# MiSTer NetBridge: lets cores with a network card (an NE2000 in the z486
# core, the MEGA65 Ethernet in the C65 core, ...) use the MiSTer's wired
# network port.
#
# The FPGA has no network port of its own: such a core keeps its frames in a
# small shared area of DDR memory and the netbridge program, running in the
# background, passes them to and from eth0. The emulated machine appears on
# your network as a machine of its own, with its own MAC address, so it gets
# its own address from your router's DHCP. The MiSTer itself reaches it too,
# through a small link interface (nbhost) the bridge sets up.
# It needs the network cable; Wi-Fi does not work for this.
#
# Run this from the Scripts menu to turn the bridge on: it starts now and at
# every boot. Run it again to turn it off. From a shell:
#   netbridge.sh on | off | status
#
# Settings go into /media/fat/Scripts/.config/netbridge/netbridge.conf:
#   NET_IF=eth1      another network port
#   HOST_LINK=no     no nbhost link between the MiSTer and the guest
#
# https://github.com/jnngill/MiSTer_NetBridge

SCRIPTS=/media/fat/Scripts
SELF=$SCRIPTS/netbridge.sh
DIR=$SCRIPTS/.config/netbridge
BIN=$DIR/netbridge
CONF=$DIR/netbridge.conf
LOG=/tmp/netbridge.log
STARTUP=/media/fat/linux/user-startup.sh
LINE='[ -f /media/fat/Scripts/netbridge.sh ] && /bin/sh /media/fat/Scripts/netbridge.sh daemon &'

NET_IF=eth0
HOST_LINK=yes
[ -f "$CONF" ] && . "$CONF"
OPTS=-v
[ "$HOST_LINK" = no ] && OPTS="-v -n"

daemon_pids() {
	ps | grep "netbridge.sh daemon" | grep -v grep | awk '{print $1}'
}

stop_all() {
	for p in $(daemon_pids); do
		[ "$p" != "$$" ] && kill "$p" 2>/dev/null
	done
	killall netbridge 2>/dev/null
	# the bridge's first version, from the C65 core
	for p in $(ps | grep "c65_net.sh daemon" | grep -v grep | awk '{print $1}'); do
		kill "$p" 2>/dev/null
	done
	killall c65_net 2>/dev/null
}

is_on() {
	[ -f "$STARTUP" ] && grep -q "netbridge.sh daemon" "$STARTUP"
}

drop_lines() {
	# take our line (and the old c65_net one) out of user-startup.sh
	[ -f "$STARTUP" ] || return
	if grep -q -e "netbridge.sh daemon" -e "c65_net.sh daemon" "$STARTUP"; then
		grep -v -e "netbridge.sh daemon" -e "c65_net.sh daemon" "$STARTUP" > "$STARTUP.tmp" &&
			cat "$STARTUP.tmp" > "$STARTUP" && rm -f "$STARTUP.tmp"
	fi
}

mtime() {
	stat -c %Y "$1" 2>/dev/null || echo 0
}

status() {
	if is_on; then echo "NetBridge is ON (starts at every boot)."; else echo "NetBridge is OFF."; fi
	if [ -n "$(daemon_pids)" ]; then echo "The bridge is running on $NET_IF."; else echo "The bridge is not running."; fi
	if [ ! -e "/sys/class/net/$NET_IF" ]; then
		echo "There is no $NET_IF: the bridge needs the wired network port."
	elif [ "$(cat /sys/class/net/$NET_IF/carrier 2>/dev/null)" != "1" ]; then
		echo "$NET_IF has no link: is the network cable plugged in?"
	fi
	[ -f "$LOG" ] && tail -n 3 "$LOG"
}

turn_on() {
	if [ ! -f "$BIN" ]; then
		echo "$BIN is missing: install NetBridge again."
		exit 1
	fi
	stop_all
	drop_lines
	[ -f "$STARTUP" ] || printf '#!/bin/sh\n' > "$STARTUP"
	echo "$LINE" >> "$STARTUP"
	/bin/sh "$SELF" daemon > /dev/null 2>&1 &
	sleep 1
	echo "NetBridge is now ON: it runs now and starts at every boot."
	echo
	echo "Cores with a network card reach your network through the"
	echo "MiSTer's network cable (z486: turn on NE2000 Network in"
	echo "the OSD; C65: MEGA65 Extensions on)."
	echo
	echo "Run this script again to turn it off."
}

turn_off() {
	stop_all
	drop_lines
	echo "NetBridge is now OFF and no longer starts at boot."
}

case "$1" in
daemon)
	# run the bridge; restart it if it stops or an update replaces it
	: > "$LOG"
	while :; do
		chmod +x "$BIN" 2>/dev/null
		t=$(mtime "$BIN")
		"$BIN" $OPTS "$NET_IF" >> "$LOG" 2>&1 &
		pid=$!
		while kill -0 "$pid" 2>/dev/null; do
			sleep 5
			if [ "$(mtime "$BIN")" != "$t" ]; then
				echo "netbridge: updated, restarting" >> "$LOG"
				kill "$pid" 2>/dev/null
				wait "$pid" 2>/dev/null
				break
			fi
		done
		sleep 2
	done
	;;
on)     turn_on ;;
off)    turn_off ;;
status) status ;;
"")
	if is_on; then turn_off; else turn_on; fi
	echo
	status
	;;
*)
	echo "usage: netbridge.sh [on | off | status]"
	exit 1
	;;
esac
exit 0
