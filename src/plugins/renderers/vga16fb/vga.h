/* vga.h - inlines for programming the VGA
 *
 * Copyright (C) 2010 Canonical Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by: Scott James Remnant <scott@ubuntu.com>
 */
#ifndef PLY_VGA_H
#define PLY_VGA_H

/* VGA ioports, and the registers we can access from them */
#define VGA_REGS_BASE		0x3c0
#define VGA_REGS_LEN 		0x10

#define VGA_SC_INDEX		0x3c4
#define VGA_SC_DATA		0x3c5

#define VGA_SC_MAP_MASK		0x02

#define VGA_GC_INDEX		0x3ce
#define VGA_GC_DATA    		0x3cf

#define VGA_GC_SET_RESET	0x00
#define VGA_GC_ENABLE_SET_RESET	0x01
#define VGA_GC_DATA_ROTATE      0x03
#define VGA_GC_MODE		0x05
#define VGA_GC_BIT_MASK		0x08

/* Select the VGA write mode. */
static inline void
vga_mode (int mode)
{
	outb (VGA_GC_MODE, VGA_GC_INDEX);
	outb (mode, VGA_GC_DATA);
}

/* Data Rotate register; we don't use this, we just ensure it's off. */
static inline void
vga_data_rotate (int op)
{
	outb (VGA_GC_DATA_ROTATE, VGA_GC_INDEX);
	outb (op, VGA_GC_DATA);
}

/* Enable use of the Set/Reset register for the given planes (as a mask).
 *
 * In effect: set this to 0xf to use all four planes.
 */
static inline void
vga_enable_set_reset (int mask)
{
	outb (VGA_GC_ENABLE_SET_RESET, VGA_GC_INDEX);
	outb (mask, VGA_GC_DATA);
}

/* Set/Reset register; the given planes (as a mask) will have whatever bits
 * are true in the Bit Mask register set to 1, and whatever bits are false
 * in the Bit Mask register set to 0.  (It's more complicated than that, but
 * your brain will explode).
 *
 * In effect: set this to the colour you want.
 */
static inline void
vga_set_reset (int mask)
{
	outb (VGA_GC_SET_RESET, VGA_GC_INDEX);
	outb (mask, VGA_GC_DATA);
}

/* Bit Mask register; writing to a memory address will write to these bits
 * of that byte according to the contents of the Set/Reset register.  Far
 * more complicated than that, you *really* don't want to know.
 *
 * In effect: set this to the pattern we want in the colour we set.
 */
static inline void
vga_bit_mask (int mask)
{
	outb (VGA_GC_BIT_MASK, VGA_GC_INDEX);
	outb (mask, VGA_GC_DATA);
}

/* Map Mask register; we don't use this, but we do make sure it's reset. */
static inline void
vga_map_mask (int mask)
{
	outb (VGA_SC_MAP_MASK, VGA_SC_INDEX);
	outb (mask, VGA_SC_DATA);
}

#endif /* PLY_VGA_H */
/* vim: set ts=4 sw=4 et ai ci cino={.5s,^-2,+.5s,t0,g0,e-2,n-2,p2s,(0,=.5s,:.5s */

