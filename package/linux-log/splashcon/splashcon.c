// SPDX-License-Identifier: GPL-2.0
// splashcon: a kernel console that draws the message log into the boot firmware's continuous-splash framebuffer, which the display keeps
// scanning out while the stock GKI kernel runs without its display driver. It is the erhai log reader's only view of itself before USB
// works. The text runs top to bottom and wraps to the top, clearing the line ahead, so a write never scrolls 31 MiB.
// Parameters: base/width/height/stride of the scanout (defaults: the live DT's cont_splash_region and the panel's 3392x2400 XRGB
// mode, as the UEFI's framebuffer PCDs), scale (the glyph magnification) and replay (1: draw the whole message buffer on registration;
// off by default: with the replay the reader restarted before anything showed, and a 2 MiB log through write-combined stores under the
// console lock is the suspect).
// On load a green bar is drawn across the top, so a mapped framebuffer shows before the first message does.
// The region is System RAM to this kernel (the reserved-memory node has no no-map), which memremap refuses to map write-combined, so
// its pages get a write-combined vmap alias: the display reads DRAM, and cached stores would sit in the cache.
#include <linux/console.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include "font8x16.h"

static unsigned long base = 0xfc800000;
static unsigned int width = 3392, height = 2400, stride = 3392, scale = 2, replay;
module_param(base, ulong, 0444);
module_param(width, uint, 0444);
module_param(height, uint, 0444);
module_param(stride, uint, 0444);
module_param(scale, uint, 0444);
module_param(replay, uint, 0444);

static u32 *fb;
static unsigned int cols, rows, col, row;

static void clear_row(unsigned int r)
{
	unsigned int y, h = 16 * scale;

	for (y = 0; y < h; y++)
		memset(fb + (size_t)(r * h + y) * stride, 0, (size_t)width * 4);
}

static void put_glyph(unsigned int c, unsigned int r, unsigned char ch)
{
	const unsigned char *g = splashcon_font + 16 * ch;
	unsigned int gy, gx, sy, sx;

	for (gy = 0; gy < 16; gy++)
		for (sy = 0; sy < scale; sy++) {
			u32 *line = fb + (size_t)(r * 16 * scale + gy * scale + sy) * stride + (size_t)c * 8 * scale;

			for (gx = 0; gx < 8; gx++) {
				u32 px = (g[gy] & (0x80 >> gx)) ? 0x00ffffff : 0;

				for (sx = 0; sx < scale; sx++)
					*line++ = px;
			}
		}
}

static void newline(void)
{
	col = 0;
	row = (row + 1) % rows;
	clear_row(row);
	clear_row((row + 1) % rows);   // a blank line marks where the newest text ends
}

static void splashcon_write(struct console *con, const char *s, unsigned int n)
{
	while (n--) {
		unsigned char ch = *s++;

		if (ch == '\n') {
			newline();
			continue;
		}
		if (ch == '\r')
			continue;
		if (col == cols)
			newline();
		put_glyph(col++, row, ch);
	}
}

static struct console splashcon = {
	.name = "splash",
	.write = splashcon_write,
	.flags = CON_ENABLED,
	.index = -1,
};

static int __init splashcon_init(void)
{
	unsigned int y, x, i, n = DIV_ROUND_UP((size_t)stride * height * 4, PAGE_SIZE);
	struct page **pages;

	if (!scale || stride < width || offset_in_page(base))
		return -EINVAL;
	pages = kvmalloc_array(n, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;
	for (i = 0; i < n; i++)
		pages[i] = pfn_to_page(PHYS_PFN(base) + i);
	fb = vmap(pages, n, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	kvfree(pages);
	if (!fb)
		return -ENOMEM;
	cols = width / (8 * scale);
	rows = height / (16 * scale);
	for (y = 0; y < 16 * scale; y++)
		for (x = 0; x < width; x++)
			fb[(size_t)y * stride + x] = 0x0000ff00;
	row = 1;
	clear_row(row);
	if (replay)
		splashcon.flags |= CON_PRINTBUFFER;
	register_console(&splashcon);
	pr_info("splashcon: %ux%u stride %u at %#lx, %u x %u cells\n", width, height, stride, base, cols, rows);
	return 0;
}

static void __exit splashcon_exit(void)
{
	unregister_console(&splashcon);
	vunmap(fb);
}

module_init(splashcon_init);
module_exit(splashcon_exit);
MODULE_DESCRIPTION("message log on the boot firmware's continuous-splash framebuffer");
MODULE_LICENSE("GPL");
