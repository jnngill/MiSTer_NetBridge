# The shared area

A core and the netbridge helper exchange frames through 12 KB of HPS DDR3 at
ARM physical address **0x3FF00000**, which is DDR word address **0x7FE0000**
on the `DDRAM_*` port (64-bit words). The area is little-endian: byte
`+n` is bits `[8*(n%8) +: 8]` of word `n/8`.

There is one area, so one core at a time uses it, which is how MiSTer works
anyway.

## Layout

| Offset | Written by | Contents |
|---|---|---|
| `+0x00` | core | `0xE7`, the area is set up |
| `+0x01` | core | protocol version: **2** (1 = the first C65/z486 releases, see below) |
| `+0x02` | core | `rx_req`: add 1 to ask for a frame |
| `+0x03` | core | `rx_slot`: RX slot to fill, 0-3 (`netbridge.sv` always uses 0) |
| `+0x04` | core | `tx_seq`: add 1 when a frame is in the TX buffer |
| `+0x05` | core | `tx_len` bits 7:0 |
| `+0x06` | core | `tx_len` bits 11:8 in bits 3:0 |
| `+0x07` | core | flags: bit 0 *running* (the NIC is started) |
| `+0x08` | helper | `rx_done`: set to `rx_req` once the frame is in the slot |
| `+0x09` | helper | `tx_done`: set to `tx_seq` once the frame is sent |
| `+0x0A` | helper | `0xA5` while the helper serves the area |
| `+0x0B` | helper | helper version (2) |
| `+0x10` | core | own MAC address, first byte at `+0x10` |
| `+0x16` | core | filter: bit 0 own address only (clear: everything), bit 4 broadcast too, bit 5 multicast too |
| `+0x18` | core | signature `"MiSTNet"` (7 bytes, `M` first) |
| `+0x1F` | core | heartbeat: changes at least every few milliseconds |
| `+0x800` | core | TX buffer, 2 KB: the frame without CRC |
| `+0x1000` | helper | RX slots 0-3, 2 KB each |

An RX slot holds a 2-byte header and then the frame, with its 4 CRC bytes:

- byte 0: length bits 7:0 (frame + CRC);
- byte 1: length bits 10:8 in bits 2:0; bit 4 multicast, bit 5 broadcast, bit 6 addressed to the own MAC.

## Handshake

The core writes word 0 (`+0x00`) whole, so `rx_req`, `tx_seq` and the length
change together. The helper writes only its own bytes.

- **Start-up.** The core reads word 1 first and carries on from the helper's
  last `rx_done`/`tx_done`, so a core reset never confuses the sequence
  numbers.
- **Transmit.** The core copies the frame to `+0x800`, then writes word 0 with
  `tx_seq + 1` and the length. The helper sends it, padding to 60 bytes, and
  sets `tx_done = tx_seq`. The core polls word 1. With no helper (no `0xA5`)
  it gives up after a short timeout, so a guest's driver never hangs.
- **Receive.** When the core has room for a frame of up to 1536 + 4 bytes, it
  writes word 0 with `rx_req + 1`. When a frame for it arrives, the helper
  writes it into slot `rx_slot` and sets `rx_done = rx_req`. The core then
  copies it out.
- **Filtering.** The helper drops frames the core would not take: its own
  frames coming back, and anything the filter byte rules out. It keeps up to
  16 waiting frames and drops the oldest when full.

## When the helper touches the area

The rest of the time, other cores may use that DDR for anything. So the
helper only reads the area until a core plainly serves it:

- **Version 2.** Byte 0 is `0xE7`, byte 1 is at least 2, the signature is in
  place, and the heartbeat byte has changed within the last second. A change
  of core (`/tmp/CORENAME`) also counts: whatever is in memory is treated as
  stale until the heartbeat moves again. A beat is a change the helper sees
  between two of its own looks, never the first value it finds, so a helper
  that starts or restarts does not count leftover memory as live. A core that stops, or a different
  core that overwrites the area, stops the helper within a second. Leftover
  memory never starts it.
- **Version 1.** Byte 0 is `0xE7`, byte 1 is 1, and the loaded core is `C65`
  or `Z486`. These are the first releases, from before the signature. New
  cores should use version 2.

`netbridge.sv` writes the signature and heartbeat on every poll, about every
32K clock cycles.
