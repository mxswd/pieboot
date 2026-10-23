/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * VideoCore firmware property interface, mailbox channel 8, as used by
 * Linux's raspberrypi firmware driver.
 *
 * Messages live in shim memory, which the VideoCore reaches through the
 * uncached bus alias (0xC0000000 + ARM physical address, below 1 GB).
 * pieboot runs with the MMU and data cache off, so no cache maintenance is
 * needed around the exchange.
 */

#include "mbox.h"

#define MBOX_READ               0x00
#define MBOX_STATUS0            0x18
#define MBOX_WRITE              0x20
#define MBOX_STATUS1            0x38
#define MBOX_FULL               0x80000000u
#define MBOX_EMPTY              0x40000000u
#define MBOX_CH_PROPERTY        8

#define MBOX_REQUEST            0
#define MBOX_RESPONSE_OK        0x80000000u

#define VC_BUS_MASK             0x3FFFFFFFu

#define TAG_GET_CLOCK_RATE      0x00030002
#define TAG_GET_DISPLAY_SIZE    0x00040003
#define TAG_ALLOCATE_BUFFER     0x00040001
#define TAG_GET_PITCH           0x00040008
#define TAG_SET_PHYS_SIZE       0x00048003
#define TAG_SET_VIRT_SIZE       0x00048004
#define TAG_SET_DEPTH           0x00048005
#define TAG_SET_PIXEL_ORDER     0x00048006
#define TAG_SET_VIRT_OFFSET     0x00048009

#define PIXEL_ORDER_BGR         0       /* blue in byte 0: 0x00RRGGBB words, xnu's BGRA bytes */

static uint64_t mbox_base;
static const struct fdt_node *mbox_node;
static volatile uint32_t msg[64] __attribute__((aligned(16)));
static uint32_t msg_bus;                /* msg's address for the firmware */

static inline uint32_t
mmio_read(uint64_t off)
{
	return *(volatile uint32_t *)(uintptr_t)(mbox_base + off);
}

static inline void
mmio_write(uint64_t off, uint32_t v)
{
	*(volatile uint32_t *)(uintptr_t)(mbox_base + off) = v;
}

bool
mbox_init(const struct fdt *fdt)
{
	struct fdt_node *n = fdt_find_compatible(fdt->root, "brcm,bcm2835-mbox");
	uint64_t size, bus;

	/*
	 * The firmware reads the message by DMA: its bus address comes from the
	 * mailbox bus's dma-ranges (the Pi 4's VideoCore sees RAM at
	 * 0xC0000000; the Pi 5's at its CPU address).
	 */
	if (!n || !fdt_reg(n, 0, &mbox_base, &size) ||
	    !fdt_dma_address(n, (uint64_t)(uintptr_t)msg, &bus) || bus + sizeof(msg) > (1ull << 32)) {
		mbox_base = 0;
		return false;
	}
	msg_bus = (uint32_t)bus;
	mbox_node = n;
	return true;
}

/* Send msg[] (msg[0] = total size) and wait for the firmware's reply. */
static bool
mbox_call(void)
{
	if (!mbox_base) {
		return false;
	}
	uint32_t word = msg_bus | MBOX_CH_PROPERTY;

	__asm__ volatile ("dsb sy" ::: "memory");
	while (mmio_read(MBOX_STATUS1) & MBOX_FULL) {
	}
	mmio_write(MBOX_WRITE, word);
	for (;;) {
		while (mmio_read(MBOX_STATUS0) & MBOX_EMPTY) {
		}
		if (mmio_read(MBOX_READ) == word) {
			break;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	return msg[1] == MBOX_RESPONSE_OK;
}

/* Append a tag with nvals 32-bit values; returns the index of its values. */
static unsigned
put_tag(unsigned *i, uint32_t tag, unsigned nvals, const uint32_t *vals)
{
	msg[(*i)++] = tag;
	msg[(*i)++] = nvals * 4;
	msg[(*i)++] = MBOX_REQUEST;
	unsigned at = *i;
	for (unsigned k = 0; k < nvals; k++) {
		msg[(*i)++] = vals ? vals[k] : 0;
	}
	return at;
}

bool
mbox_framebuffer(struct Boot_Video *video)
{
	unsigned i = 2;

	/* The display size the firmware chose (from HDMI EDID or config.txt). */
	unsigned at = put_tag(&i, TAG_GET_DISPLAY_SIZE, 2, NULL);
	msg[i++] = 0;
	msg[0] = i * 4;
	msg[1] = MBOX_REQUEST;
	if (!mbox_call()) {
		return false;
	}
	uint32_t width = msg[at], height = msg[at + 1];
	if (width == 0 || height == 0) {
		width = 1024;
		height = 768;
	}

	i = 2;
	const uint32_t size[2] = { width, height };
	const uint32_t zero2[2] = { 0, 0 };
	const uint32_t depth = 32, order = PIXEL_ORDER_BGR, align[2] = { 4096, 0 };
	put_tag(&i, TAG_SET_PHYS_SIZE, 2, size);
	put_tag(&i, TAG_SET_VIRT_SIZE, 2, size);
	put_tag(&i, TAG_SET_VIRT_OFFSET, 2, zero2);
	unsigned at_depth = put_tag(&i, TAG_SET_DEPTH, 1, &depth);
	put_tag(&i, TAG_SET_PIXEL_ORDER, 1, &order);
	unsigned at_buf = put_tag(&i, TAG_ALLOCATE_BUFFER, 2, align);
	unsigned at_pitch = put_tag(&i, TAG_GET_PITCH, 1, NULL);
	msg[i++] = 0;
	msg[0] = i * 4;
	msg[1] = MBOX_REQUEST;
	if (!mbox_call() || msg[at_buf] == 0 || msg[at_pitch] == 0) {
		return false;
	}
	/*
	 * The depth it really has: the Pi 5's firmware answers 32 but keeps its
	 * own 16 bpp framebuffer (the pitch tells).
	 */
	uint32_t bpp = msg[at_pitch] * 8 / width;
	if (bpp != 16 && bpp != 32) {
		bpp = msg[at_depth];
	}
	if (bpp != 16 && bpp != 32) {
		return false;
	}

	/* The buffer's bus address, back to the CPU's (else the Pi 4's alias). */
	uint64_t fb;
	if (!fdt_dma_to_cpu(mbox_node, msg[at_buf], &fb)) {
		fb = msg[at_buf] & VC_BUS_MASK;
	}
	video->v_baseAddr = fb;
	video->v_rowBytes = msg[at_pitch];
	video->v_width = width;
	video->v_height = height;
	video->v_depth = bpp;           /* no rotation, 1x scale */
	return true;
}

uint32_t
mbox_clock_rate(uint32_t clock_id)
{
	unsigned i = 2;
	const uint32_t req[2] = { clock_id, 0 };
	unsigned at = put_tag(&i, TAG_GET_CLOCK_RATE, 2, req);
	msg[i++] = 0;
	msg[0] = i * 4;
	msg[1] = MBOX_REQUEST;
	if (!mbox_call() || msg[at] != clock_id) {
		return 0;
	}
	return msg[at + 1];
}
