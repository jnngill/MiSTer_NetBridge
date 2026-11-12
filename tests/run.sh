#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Runs the testbench with Icarus Verilog (12 or newer).
set -e
cd "$(dirname "$0")"
iverilog -g2012 -o tb_ne2000.vvp tb_ne2000.sv \
	../hdl/netbridge.sv ../hdl/netbridge_ne2000.sv ../hdl/netbridge_dpram.sv ../hdl/netbridge_ddram_arb.sv
vvp -n tb_ne2000.vvp | tee tb_ne2000.log
grep -q '^PASS' tb_ne2000.log
