# The helper for the MiSTer's ARM: a static binary, no libraries needed there.
# With zig (any OS):           make
# With an ARM gcc toolchain:   make CC=arm-linux-gnueabihf-gcc CFLAGS="-O2 -s -static"
CC     ?= zig cc -target arm-linux-musleabihf
CFLAGS ?= -O2 -s -static -Wall -Wextra
OUT    := ../Scripts/.config/netbridge/netbridge

$(OUT): netbridge.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	rm -f $(OUT)
