# MiSTer NetBridge

NetBridge gives MiSTer cores real networking through the DE10-Nano's wired
network port.

The FPGA has no network port of its own; the MiSTer's Linux side has. A core
with a network card keeps its frames in a small shared area of DDR memory,
and a small helper program on the Linux side passes them to and from `eth0`.
The emulated machine is a machine of its own on your network. It has its own
MAC address, takes an address from your router's DHCP, and reaches the
internet and other machines.

This repository holds both halves:

- the helper and its install script, for MiSTer users;
- reusable HDL for core developers: the bridge itself, an NE2000 card built
  on it, and an arbiter for sharing the DDR port.

## Cores that use it

| Core | Network hardware | Turn it on |
|---|---|---|
| [C65](https://github.com/jnngill/C65_MiSTer) | MEGA65 Ethernet controller | OSD: MEGA65 Extensions |
| [z486](https://github.com/jnngill/z486_MiSTer) | NE2000 at 300h, IRQ 3 or 9 | OSD: Hardware → NE2000 Network |

## For MiSTer users

### Install with update_all / Downloader

Add this to the bottom of `/media/fat/downloader.ini`, then run update_all or
`downloader`:

```ini
[mister_netbridge]
db_url = https://raw.githubusercontent.com/jnngill/MiSTer_NetBridge/main/db.json
```

It installs `Scripts/netbridge.sh` and the helper (in
`Scripts/.config/netbridge/`), and keeps them up to date.

### Or by hand

Copy `Scripts/netbridge.sh` to `/media/fat/Scripts/`, and
`Scripts/.config/netbridge/netbridge` to
`/media/fat/Scripts/.config/netbridge/`.

### Turn it on

Run **netbridge** from the Scripts menu. The bridge starts now and at every
boot. Run it again to turn it off. From a shell:

```sh
/media/fat/Scripts/netbridge.sh on
/media/fat/Scripts/netbridge.sh off
/media/fat/Scripts/netbridge.sh status
```

Then turn on the network hardware in the core's OSD. With no network core
loaded, the helper waits and touches nothing.

**Updates:** Downloader replaces the helper in place, and the running bridge
restarts on the new version within a few seconds.

**From c65_net:** if you ran `c65_net.sh` from the C65 core before, turning
NetBridge on stops it and removes its line from `user-startup.sh`.
NetBridge serves the C65 just the same.

### Things to know

- **Cable only.** The MiSTer must be on the network cable. Wi-Fi cannot do
  this.
- **The MiSTer reaches the guest too.** Frames sent out of `eth0` never come
  back in to the MiSTer's own Linux, so the helper links the two itself: a
  small point-to-point interface, `nbhost`, carries IPv4 between them. The
  helper answers the guest's ARP for the MiSTer's address and routes the
  guest's address (learned from its traffic) through `nbhost`. So the guest
  can fetch files from a server on the MiSTer, and the MiSTer can connect to
  the guest. `eth0`, its address and routes are not touched, and nothing
  needs a bridge in the kernel. To leave it out, put `HOST_LINK=no` into
  `netbridge.conf` (below). IPv4 only.
- **Another interface.** Put `NET_IF=eth1` (for example) into
  `/media/fat/Scripts/.config/netbridge/netbridge.conf`.
- **GRO is turned off.** The helper turns off Generic Receive Offload on the
  interface. Merged TCP segments are bigger than old network stacks accept.
- **Log.** `/tmp/netbridge.log` shows when a core attaches and detaches.

## For core developers

**Adding it to a core:** see [docs/INTEGRATION.md](docs/INTEGRATION.md). In
short:

- **PC-style cores:** drop in `netbridge_ne2000.sv`, decode 20h I/O ports,
  wire up an IRQ, and connect its DDR port.
- **Other network hardware:** wrap your model around `netbridge.sv`, which
  turns the shared area into simple TX and RX byte streams.
- **Sharing DDR:** `netbridge_ddram_arb.sv` shares `DDRAM_*` with a core's
  own DDR traffic.

**The protocol:** [docs/PROTOCOL.md](docs/PROTOCOL.md) gives the shared-area
layout and handshake, if you would rather write your own FPGA side. The C65
core does this: its MEGA65 Ethernet controller
([`rtl/m65_eth.sv`](https://github.com/jnngill/C65_MiSTer/blob/master/rtl/m65_eth.sv))
keeps its four RX buffers in the shared area itself.

| File | |
|---|---|
| `hdl/netbridge.sv` | the bridge: DDR shared area ↔ TX/RX byte streams |
| `hdl/netbridge_ne2000.sv` | NE2000 (DP8390, 16K buffer, word-mode data port) on the bridge |
| `hdl/netbridge_ddram_arb.sv` | shares a `DDRAM_*` port between a core's master and the bridge |
| `hdl/netbridge_dpram.sv` | the NE2000's buffer RAM |
| `hdl/netbridge.qip` | all of the above for Quartus |
| `helper/netbridge.c` | the ARM helper (`make` builds it with zig) |
| `Scripts/netbridge.sh` | install/toggle script and the helper's supervisor |
| `tests/` | testbench (`sh tests/run.sh`, Icarus Verilog) |
| `tools/make_db.py` | writes `db.json` for Downloader |

**Releasing:** after changing anything in `Scripts/`, run
`python3 tools/make_db.py` and commit `db.json` with it. Before pushing,
`sh tests/run.sh` and `python3 tools/make_db.py --check` should both pass
(Downloader refuses a file whose hash doesn't match `db.json`).

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
