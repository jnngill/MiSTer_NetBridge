# Adding networking to a core

There are two ways in:

- **The NE2000 card** (`netbridge_ne2000.sv`), for cores with an ISA-style I/O
  bus: PCs (DOS, Windows 3.x/9x, Linux, Minix), and anything else NE2000
  drivers exist for.
- **The bridge on its own** (`netbridge.sv`), for any other network
  hardware a core models. The MEGA65 Ethernet controller, an Amiga or
  Atari ST network card and a CS8900A are examples. Your model keeps its own
  registers and buffer, and the bridge moves frames between that buffer and
  the helper.

The DDR side is the same either way.

## Files

Copy `hdl/` into the core, or add this repository as a git submodule. Then
add `hdl/netbridge.qip` to the project's file list:

```
set_global_assignment -name QIP_FILE rtl/netbridge/netbridge.qip
```

All files are plain SystemVerilog with no vendor primitives. The RAM in
`netbridge_dpram.sv` infers block RAM. The NE2000's 16 KB buffer takes
16 M10K blocks; the bridge alone takes none.

## Clock

The bridge's `clk` must be the clock of the `DDRAM_*` interface it drives.
That is the `DDRAM_CLK` the core gives the framework, usually `clk_sys`. The
I/O bus of the NE2000 is on the same clock. Anything from 20 to 120 MHz
works.

## DDR: a core that does not use DDRAM

Connect the bridge (or the NE2000's `ddr_*` port) straight to the framework:

```verilog
assign DDRAM_CLK      = clk_sys;
assign DDRAM_BURSTCNT = 8'd1;

netbridge_ne2000 nic (
    .clk(clk_sys), .reset(reset), .enable(status[42]),   // an OSD switch
    .io_address(io_addr[4:0]), .io_read(io_rd), .io_write(io_wr),
    .io_cs(io_addr[15:5] == 11'h018),                      // 300h-31Fh
    .io_writedata(io_dout), .io_readdata(nic_dout), .irq(nic_irq),
    .ddr_addr(DDRAM_ADDR), .ddr_rd(DDRAM_RD), .ddr_we(DDRAM_WE),
    .ddr_din(DDRAM_DIN), .ddr_be(DDRAM_BE),
    .ddr_busy(DDRAM_BUSY), .ddr_dout(DDRAM_DOUT), .ddr_dout_ready(DDRAM_DOUT_READY)
);
```

## DDR: a core that already uses DDRAM

Put `netbridge_ddram_arb.sv` between the core's own DDR master (A, bursts
allowed) and the bridge (B):

```verilog
netbridge_ddram_arb ddr_arb (
    .clk(clk_sys), .reset(reset),
    .a_addr(core_ddr_addr), .a_burstcnt(core_ddr_burstcnt), .a_rd(core_ddr_rd), .a_we(core_ddr_we),
    .a_din(core_ddr_din), .a_be(core_ddr_be), .a_busy(core_ddr_busy), .a_dout_ready(core_ddr_dout_ready),
    .b_addr(nic_ddr_addr), .b_rd(nic_ddr_rd), .b_we(nic_ddr_we), .b_din(nic_ddr_din), .b_be(nic_ddr_be),
    .b_busy(nic_ddr_busy), .b_dout_ready(nic_ddr_dout_ready),
    .ddr_addr(DDRAM_ADDR), .ddr_burstcnt(DDRAM_BURSTCNT), .ddr_rd(DDRAM_RD), .ddr_we(DDRAM_WE),
    .ddr_din(DDRAM_DIN), .ddr_be(DDRAM_BE), .ddr_busy(DDRAM_BUSY), .ddr_dout_ready(DDRAM_DOUT_READY)
);
// DDRAM_DOUT goes to both masters as it is
```

- **Hand-over.** The port changes hands only between transactions. While the
  bridge wants a word, A's new requests wait.
- **Bandwidth.** The bridge uses little: two words every 32K cycles when idle,
  and about one word every 8-16 cycles while a frame moves.
- **Address range.** Keep the core's own DDR use clear of
  `0x3FF00000`-`0x3FF02FFF` (word addresses `0x7FE0000`-`0x7FE05FF`).
- **Boot-time loaders.** If a loader owns DDRAM at times, as in the z486, put
  the arbiter after the mux that hands the port over, or hold the bridge in
  reset until the core has booted.

## The NE2000

- **Bus.** The I/O bus is byte-wide. `io_cs` selects the card's 20h ports,
  and `io_read`/`io_write` are one-cycle strobes with `io_address[4:0]`
  stable. `io_readdata` is registered on the `io_read` edge, which is also
  when the side effects happen: a data port read advances the address. A
  16-bit IN/OUT on the data port must arrive as two byte accesses, 10h then
  11h. A bus adapter that splits words does exactly this.
- **IRQ.** `irq` is a level. Route it to the IRQ the user picks: 3, 5, 9, 10
  or 11 are usual. Offer the base and IRQ in the OSD, and leave the card off
  by default.
- **MAC address.** Use a locally administered one (the first byte has bit 1
  set and bit 0 clear), different for each core. The default
  `02:4E:45:32:4B:01` is the z486's. Two MiSTers running the same core on one
  network collide, so a core that may meet that case can offer a few
  addresses in the OSD.
- **Drivers.**
  - The Crynwr `NE2000` packet driver works with mTCP, WATTCP programs and
    the like, e.g. `NE2000 0x60 3 0x300`.
  - Windows 9x has its own NE2000 driver.
  - Linux uses `ne`, e.g. `modprobe ne io=0x300 irq=3`.

## Your own network hardware

Instantiate `netbridge` and connect it to your model's frame buffer. The
header of `netbridge.sv` describes the ports. In short:

- **Configuration.** Drive `running`, `mac` and `filter` from your registers.
  The bridge republishes them when they change.
- **Transmit.**
  - Pulse `tx_start` with `tx_len` (1-2047, no CRC).
  - The bridge raises `tx_rd` with `tx_addr` and expects that byte on
    `tx_data` in the next cycle, as from a block RAM.
  - `tx_done` pulses when the helper has sent the frame, or after a timeout
    with no helper.
- **Receive.**
  - Hold `rx_ready` high while a whole frame (1540 bytes) fits.
  - `rx_start` comes with `rx_len` (including the 4 CRC bytes) and the
    broadcast/multicast flags.
  - Then each byte comes with `rx_we`, `rx_addr` and `rx_data`, one a cycle
    at most, and finally `rx_end`.
  - You must take every byte. Decide whether to keep a frame at `rx_start`.
- **hold.** While `hold` is high, the bridge starts no new copy, which keeps
  your buffer port free. For example, the NE2000 writes its ring header after
  `rx_end`. `hold` must already be high in the cycle `rx_end` is.

For anything beyond this, the protocol is in [PROTOCOL.md](PROTOCOL.md).

## Testing

`tests/run.sh` runs the NE2000 testbench in Icarus Verilog. It covers the
bridge, the arbiter with a second master doing bursts, and a model of the
helper.

On the MiSTer, `/tmp/netbridge.log` shows when a core attaches and detaches.
