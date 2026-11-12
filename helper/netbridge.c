/* SPDX-License-Identifier: GPL-2.0-or-later */
/* netbridge: the ARM side of MiSTer NetBridge.
 *
 * The DE10-Nano's FPGA has no network port of its own. A core with a network
 * card keeps a small shared area in DDR3 at 0x3FF00000 (hdl/netbridge.sv)
 * and this program moves Ethernet frames between it and the MiSTer's wired
 * port with a raw packet socket. The emulated machine is then a machine of
 * its own on the LAN, with its own MAC address: DHCP, the internet and other
 * machines work. One thing does not: the MiSTer's own Linux cannot talk to
 * it, because frames sent out of eth0 do not come back in.
 *
 * Other cores may use that DDR for anything, so this only touches the area
 * while a core is plainly serving it (docs/PROTOCOL.md):
 *   version 2: the "MiSTNet" signature is there and its heartbeat byte has
 *     changed in the last second, since the last core change;
 *   version 1 (the C65 and z486 cores' first releases): the loaded core
 *     (/tmp/CORENAME) is one of those.
 *
 * usage: netbridge [-v] [interface]   (default eth0; runs in the foreground)
 *
 * build: zig cc -target arm-linux-musleabihf -O2 -s -static -o netbridge netbridge.c
 */
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SHM_PHYS   0x3FF00000u
#define SHM_SIZE   0x3000u
#define QMAX       16
#define HELPER_VER 2

/* offsets in the shared area */
#define S_MAGIC   0x00   /* 0xE7, written by the core */
#define S_VER     0x01
#define S_RXREQ   0x02
#define S_RXSLOT  0x03
#define S_TXSEQ   0x04
#define S_TXLEN   0x05   /* 12 bits */
#define S_FLAGS   0x07   /* bit 0 running */
#define S_RXDONE  0x08   /* written here */
#define S_TXDONE  0x09
#define S_ALIVE   0x0A   /* 0xA5 while this runs */
#define S_HVER    0x0B
#define S_MAC     0x10   /* written by the core */
#define S_FILTER  0x16
#define S_SIG     0x18   /* "MiSTNet" and the heartbeat byte */
#define S_BEAT    0x1F
#define S_TXBUF   0x800
#define S_RXBUF   0x1000

static volatile uint8_t *shm;
static volatile int quit;
static int verbose;
static unsigned char q[QMAX][2048];
static int qlen[QMAX], qhead, qn;

static void on_signal(int s) { (void)s; quit = 1; }

static long now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void core_name(char *b, int n) {
	FILE *f = fopen("/tmp/CORENAME", "r");
	b[0] = 0;
	if (!f) return;
	if (!fgets(b, n, f)) b[0] = 0;
	fclose(f);
	b[strcspn(b, "\r\n")] = 0;
}

static int legacy_core(const char *n) {
	/* cores that shipped with version 1 of the area, before the signature */
	return !strcmp(n, "C65") || !strcmp(n, "Z486");
}

static int sig_ok(void) {
	static const char sig[7] = { 'M', 'i', 'S', 'T', 'N', 'e', 't' };
	for (int i = 0; i < 7; i++) if (shm[S_SIG + i] != (uint8_t)sig[i]) return 0;
	return 1;
}

static uint32_t crc32(const unsigned char *d, int n) {
	uint32_t c = 0xFFFFFFFFu;
	for (int i = 0; i < n; i++) {
		c ^= d[i];
		for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
	}
	return ~c;
}

int main(int argc, char **argv) {
	const char *ifname = "eth0";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) verbose = 1;
		else ifname = argv[i];
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	int mfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (mfd < 0) { perror("/dev/mem"); return 1; }
	shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, SHM_PHYS);
	if (shm == MAP_FAILED) { perror("mmap"); return 1; }

	int s = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (s < 0) { perror("socket"); return 1; }
	int ifindex = if_nametoindex(ifname);
	if (!ifindex) { fprintf(stderr, "%s: no such interface\n", ifname); return 1; }
	struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = ifindex };
	if (bind(s, (struct sockaddr *)&sll, sizeof sll) < 0) { perror("bind"); return 1; }
	struct packet_mreq mr = { .mr_ifindex = ifindex, .mr_type = PACKET_MR_PROMISC };
	setsockopt(s, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof mr);
	fcntl(s, F_SETFL, O_NONBLOCK);
	/* Generic receive offload merges TCP segments arriving back to back into
	 * one frame larger than any the sender sent, and a packet socket gets the
	 * merged frame: too big for the guest (a DOS stack with a 576-byte MTU
	 * drops it). Hand over frames as they were on the wire. */
	{
		struct ethtool_value ev = { .cmd = ETHTOOL_SGRO, .data = 0 };
		struct ifreq ifr;
		memset(&ifr, 0, sizeof ifr);
		strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
		ifr.ifr_data = (void *)&ev;
		if (ioctl(s, SIOCETHTOOL, &ifr) < 0) perror("GRO off");
	}
	fprintf(stderr, "netbridge: %s, shared area at 0x%08X\n", ifname, SHM_PHYS);

	char core[64] = "", prev_core[64] = "";
	int active = 0, legacy = 0, beat_seen = 0;
	uint8_t last_beat = 0;
	long last_beat_ms = 0, last_name_ms = 0;
	while (!quit) {
		struct pollfd p = { .fd = s, .events = POLLIN };
		poll(&p, 1, 1);
		long now = now_ms();

		/* which core, and is it serving the area */
		if (now - last_name_ms >= 100) {
			last_name_ms = now;
			core_name(core, sizeof core);
			if (strcmp(core, prev_core)) {
				/* a new core: whatever is in the area is stale until it beats */
				strcpy(prev_core, core);
				beat_seen = 0;
				legacy = legacy_core(core);
			}
		}
		if (sig_ok() && shm[S_BEAT] != last_beat) {
			last_beat = shm[S_BEAT];
			last_beat_ms = now;
			beat_seen = 1;
		}
		int was = active;
		if (shm[S_MAGIC] != 0xE7) active = 0;
		else if (shm[S_VER] >= 2) active = beat_seen && sig_ok() && now - last_beat_ms < 1000;
		else active = legacy && shm[S_VER] == 1;
		if (active != was) {
			if (!active) qn = 0;
			if (verbose) fprintf(stderr, "netbridge: %s %s (area version %d)\n",
			                     core, active ? "attached" : "detached", shm[S_VER]);
		}

		/* everything that arrived: keep what the core would accept */
		unsigned char buf[4096];
		struct sockaddr_ll from;
		socklen_t fl = sizeof from;
		int n;
		while ((n = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl)) > 0) {
			fl = sizeof from;
			if (!active || !(shm[S_FLAGS] & 1)) continue;
			if (from.sll_pkttype == PACKET_OUTGOING || n < 14 || n > 2042) continue;
			uint8_t mac[6], filter = shm[S_FILTER];
			for (int i = 0; i < 6; i++) mac[i] = shm[S_MAC + i];
			if (!memcmp(buf + 6, mac, 6)) continue;            /* our own frame */
			int bc = !memcmp(buf, "\xFF\xFF\xFF\xFF\xFF\xFF", 6);
			int mc = (buf[0] & 1) && !bc;
			int me = !memcmp(buf, mac, 6);
			if ((filter & 0x01) && !me && !(bc && (filter & 0x10)) && !(mc && (filter & 0x20))) continue;
			if (qn == QMAX) { qhead = (qhead + 1) % QMAX; qn--; }  /* drop the oldest */
			int t = (qhead + qn) % QMAX;
			memcpy(q[t], buf, n);
			qlen[t] = n;
			qn++;
		}
		if (!active) continue;
		shm[S_ALIVE] = 0xA5;
		shm[S_HVER] = HELPER_VER;
		if (!(shm[S_FLAGS] & 1)) continue;

		/* a frame from the core */
		uint8_t tx_seq = shm[S_TXSEQ];
		if (tx_seq != shm[S_TXDONE]) {
			int len = shm[S_TXLEN] | ((shm[S_TXLEN + 1] & 0x0F) << 8);
			unsigned char f[2048];
			for (int i = 0; i < len; i++) f[i] = shm[S_TXBUF + i];
			if (len < 60) { memset(f + len, 0, 60 - len); len = 60; }
			sendto(s, f, len, 0, (struct sockaddr *)&sll, sizeof sll);
			__sync_synchronize();
			shm[S_TXDONE] = tx_seq;
		}

		/* the core wants a frame and one is waiting */
		uint8_t rx_req = shm[S_RXREQ];
		if (rx_req != shm[S_RXDONE] && qn) {
			unsigned char *f = q[qhead];
			int len = qlen[qhead];
			qhead = (qhead + 1) % QMAX; qn--;
			uint32_t crc = crc32(f, len);
			for (int k = 0; k < 4; k++) f[len + k] = crc >> (8 * k);
			len += 4;
			uint8_t mac[6];
			for (int i = 0; i < 6; i++) mac[i] = shm[S_MAC + i];
			int bc = !memcmp(f, "\xFF\xFF\xFF\xFF\xFF\xFF", 6);
			int mc = (f[0] & 1) && !bc;
			int me = !memcmp(f, mac, 6);
			volatile uint8_t *b = shm + S_RXBUF + (shm[S_RXSLOT] & 3) * 0x800;
			b[0] = len & 0xFF;
			b[1] = ((len >> 8) & 7) | (me << 6) | (bc << 5) | (mc << 4);
			for (int i = 0; i < len; i++) b[2 + i] = f[i];
			__sync_synchronize();
			shm[S_RXDONE] = rx_req;
		}
	}
	if (active) shm[S_ALIVE] = 0;
	return 0;
}
