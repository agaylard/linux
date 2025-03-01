// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO Driver for Sunplus/Tibbo SP7021 controller
 * Copyright (C) 2020 Sunplus Tech./Tibbo Tech.
 * Author: Dvorkin Dmitry <dvorkin@tibbo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/seq_file.h>
#include <linux/io.h>

#include "sppctl_gpio.h"
#include "sppctl_gpio_ops.h"

#ifdef CONFIG_PINCTRL_SPPCTL
#define SUPPORT_PINMUX
#endif

#define SPPCTL_GPIO_OFF_GFR     0x00
#ifdef CONFIG_PINCTRL_SPPCTL_Q645
#define SPPCTL_GPIO_OFF_CTL     0x00
#define SPPCTL_GPIO_OFF_OE      0x34
#define SPPCTL_GPIO_OFF_OUT     0x68
#define SPPCTL_GPIO_OFF_IN      0x9c
#define SPPCTL_GPIO_OFF_IINV    0xbc
#define SPPCTL_GPIO_OFF_OINV    0xf0
#define SPPCTL_GPIO_OFF_OD      0x124
#else
#define SPPCTL_GPIO_OFF_CTL     0x00
#define SPPCTL_GPIO_OFF_OE      0x20
#define SPPCTL_GPIO_OFF_OUT     0x40
#define SPPCTL_GPIO_OFF_IN      0x60
#define SPPCTL_GPIO_OFF_IINV    0x00
#define SPPCTL_GPIO_OFF_OINV    0x20
#define SPPCTL_GPIO_OFF_OD      0x40
#endif

// (/16)*4
#define R16_ROF(r)              (((r)>>4)<<2)
#define R16_BOF(r)              ((r)%16)
// (/32)*4
#define R32_ROF(r)              (((r)>>5)<<2)
#define R32_BOF(r)              ((r)%32)
#define R32_VAL(r, boff)        (((r)>>(boff)) & BIT(0))

void sppctlgpio_unmux_irq( struct gpio_chip *_c, unsigned _pin);

// who is first: GPIO(1) | MUX(0)
int sppctlgpio_u_gfrst(struct gpio_chip *_c, unsigned int _n)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

	r = readl(pc->base2 + SPPCTL_GPIO_OFF_GFR + R32_ROF(_n));
	//KINF(_c->parent, "u F r:%X = %d %px off:%d\n", r, R32_VAL(r,R32_BOF(_n)),
	//	pc->base2, SPPCTL_GPIO_OFF_GFR + R32_ROF(_n));

	return R32_VAL(r, R32_BOF(_n));
}

// who is master: GPIO(1) | IOP(0)
int sppctlgpio_u_magpi(struct gpio_chip *_c, unsigned int _n)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

	r = readl(pc->base0 + SPPCTL_GPIO_OFF_CTL + R16_ROF(_n));
	//KINF(_c->parent, "u M r:%X = %d %px off:%d\n", r, R32_VAL(r,R16_BOF(_n)),
	//	pc->base0, SPPCTL_GPIO_OFF_CTL + R16_ROF(_n));

	return R32_VAL(r, R16_BOF(_n));
}

// set master: GPIO(1)|IOP(0), first:GPIO(1)|MUX(0)
void sppctlgpio_u_magpi_set(struct gpio_chip *_c, unsigned int _n, enum muxF_MG_t _f,
			    enum muxM_IG_t _m)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

	// FIRST
	if (_f != muxFKEEP) {
		r = readl(pc->base2 + SPPCTL_GPIO_OFF_GFR + R32_ROF(_n));
		//KINF(_c->parent, "F r:%X %px off:%d\n", r, pc->base2,
		//	SPPCTL_GPIO_OFF_GFR + R32_ROF(_n));
		if (_f != R32_VAL(r, R32_BOF(_n))) {
			if (_f == muxF_G)
				r |= BIT(R32_BOF(_n));
			else
				r &= ~BIT(R32_BOF(_n));
			//KINF(_c->parent, "F w:%X\n", r);
			writel(r, pc->base2 + SPPCTL_GPIO_OFF_GFR + R32_ROF(_n));
		}
	}

	// MASTER
	if (_m != muxMKEEP) {
		r = (BIT(R16_BOF(_n))<<16);
		if (_m == muxM_G)
			r |= BIT(R16_BOF(_n));
		//KINF(_c->parent, "M w:%X %px off:%d\n", r, pc->base0,
		//	SPPCTL_GPIO_OFF_CTL + R16_ROF(_n));
		writel(r, pc->base0 + SPPCTL_GPIO_OFF_CTL + R16_ROF(_n));
	}
}

// is inv: INVERTED(1) | NORMAL(0)
int sppctlgpio_u_isinv(struct gpio_chip *_c, unsigned int _n)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);
	u16 inv_off = SPPCTL_GPIO_OFF_IINV;

	if (sppctlgpio_f_gdi(_c, _n) == 0)
		inv_off = SPPCTL_GPIO_OFF_OINV;

#ifdef CONFIG_PINCTRL_SPPCTL
	r = readl(pc->base1 + inv_off + R16_ROF(_n));
#else
	r = readl(pc->base0 + inv_off + R16_ROF(_n));
#endif

	return R32_VAL(r, R16_BOF(_n));
}

void sppctlgpio_u_siinv(struct gpio_chip *_c, unsigned int _n)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);
	u16 inv_off = SPPCTL_GPIO_OFF_IINV;

	r = (BIT(R16_BOF(_n))<<16) | BIT(R16_BOF(_n));
#ifdef CONFIG_PINCTRL_SPPCTL
	writel(r, pc->base1 + inv_off + R16_ROF(_n));
#else
	writel(r, pc->base0 + inv_off + R16_ROF(_n));
#endif
}

void sppctlgpio_u_soinv(struct gpio_chip *_c, unsigned int _n)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);
	u16 inv_off = SPPCTL_GPIO_OFF_OINV;

	r = (BIT(R16_BOF(_n))<<16) | BIT(R16_BOF(_n));
#ifdef CONFIG_PINCTRL_SPPCTL
	writel(r, pc->base1 + inv_off + R16_ROF(_n));
#else
	writel(r, pc->base0 + inv_off + R16_ROF(_n));
#endif
}

// is open-drain: YES(1) | NON(0)
int sppctlgpio_u_isodr(struct gpio_chip *_c, unsigned int _n)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

#ifdef CONFIG_PINCTRL_SPPCTL
	r = readl(pc->base1 + SPPCTL_GPIO_OFF_OD + R16_ROF(_n));
#else
	r = readl(pc->base0 + SPPCTL_GPIO_OFF_OD + R16_ROF(_n));
#endif

	return R32_VAL(r, R16_BOF(_n));
}

void sppctlgpio_u_seodr(struct gpio_chip *_c, unsigned int _n, unsigned int _v)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

	r = (BIT(R16_BOF(_n))<<16) | ((_v & BIT(0)) << R16_BOF(_n));
#ifdef CONFIG_PINCTRL_SPPCTL
	writel(r, pc->base1 + SPPCTL_GPIO_OFF_OD + R16_ROF(_n));
#else
	writel(r, pc->base0 + SPPCTL_GPIO_OFF_OD + R16_ROF(_n));
#endif
}

#ifndef SPPCTL_H
// take pin (export/open for ex.): set GPIO_FIRST=1,GPIO_MASTER=1
// FIX: how to prevent gpio to take over the mux if mux is the default?
// FIX: idea: save state of MASTER/FIRST and return back after _fre?
int sppctlgpio_f_req(struct gpio_chip *_c, unsigned int _n)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

	//KINF(_c->parent, "f_req(%03d)\n", _n);
	// get GPIO_FIRST:32
	r = readl(pc->base2 + SPPCTL_GPIO_OFF_GFR + R32_ROF(_n));
	// set GPIO_FIRST(1):32
	r |= BIT(R32_BOF(_n));
	writel(r, pc->base2 + SPPCTL_GPIO_OFF_GFR + R32_ROF(_n));

	// set GPIO_MASTER(1):m16,v:16
	r = (BIT(R16_BOF(_n))<<16) | BIT(R16_BOF(_n));
	writel(r, pc->base0 + SPPCTL_GPIO_OFF_CTL + R16_ROF(_n));

	return 0;
}

// gave pin back: set GPIO_MASTER=0,GPIO_FIRST=0
void sppctlgpio_f_fre(struct gpio_chip *_c, unsigned int _n)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

	// set GPIO_MASTER(1):m16,v:16 - doesn't matter now: gpio mode is default
	//r = (BIT(R16_BOF(_n))<<16) | BIT(R16_BOF(_n);
	//writel(r, pc->base0 + SPPCTL_GPIO_OFF_CTL + R16_ROF(_n));

	// get GPIO_FIRST:32
	r = readl(pc->base2 + SPPCTL_GPIO_OFF_GFR + R32_ROF(_n));
	// set GPIO_FIRST(0):32
	r &= ~BIT(R32_BOF(_n));
	writel(r, pc->base2 + SPPCTL_GPIO_OFF_GFR + R32_ROF(_n));
}
#endif // SPPCTL_H

// get dir: 0=out, 1=in, -E =err (-EINVAL for ex): OE inverted on ret
int sppctlgpio_f_gdi(struct gpio_chip *_c, unsigned int _n)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

	r = readl(pc->base0 + SPPCTL_GPIO_OFF_OE + R16_ROF(_n));

	return R32_VAL(r, R16_BOF(_n)) ^ BIT(0);
}

// set to input: 0:ok: OE=0
int sppctlgpio_f_sin(struct gpio_chip *_c, unsigned int _n)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

	r = (BIT(R16_BOF(_n))<<16);
	writel(r, pc->base0 + SPPCTL_GPIO_OFF_OE + R16_ROF(_n));

	return 0;
}

// set to output: 0:ok: OE=1,O=_v
int sppctlgpio_f_sou(struct gpio_chip *_c, unsigned int _n, int _v)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

	r = (BIT(R16_BOF(_n))<<16) | BIT(R16_BOF(_n));
	writel(r, pc->base0 + SPPCTL_GPIO_OFF_OE + R16_ROF(_n));
	if (_v < 0)
		return 0;
	r = (BIT(R16_BOF(_n))<<16) | ((_v & BIT(0)) << R16_BOF(_n));
	writel(r, pc->base0 + SPPCTL_GPIO_OFF_OUT + R16_ROF(_n));
	sppctlgpio_unmux_irq( _c, _n);

	return 0;
}

// get value for signal: 0=low | 1=high | -err
int sppctlgpio_f_get(struct gpio_chip *_c, unsigned int _n)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

	r = readl(pc->base0 + SPPCTL_GPIO_OFF_IN + R32_ROF(_n));

	return R32_VAL(r, R32_BOF(_n));
}

// OUT only: can't call set on IN pin: protected by gpio_chip layer
void sppctlgpio_f_set(struct gpio_chip *_c, unsigned int _n, int _v)
{
	u32 r;
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

	r = (BIT(R16_BOF(_n))<<16) | (_v & 0x0001) << R16_BOF(_n);
	writel(r, pc->base0 + SPPCTL_GPIO_OFF_OUT + R16_ROF(_n));
}

// FIX: test in-depth
int sppctlgpio_f_scf(struct gpio_chip *_c, unsigned int _n, unsigned long _conf)
{
	u32 r;
	int ret = 0;
	enum pin_config_param cp = pinconf_to_config_param(_conf);
	u16 ca = pinconf_to_config_argument(_conf);
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);

	KDBG(_c->parent, "f_scf(%03d,%lX) p:%d a:%d\n", _n, _conf, cp, ca);
	switch (cp) {
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		r = (BIT(R16_BOF(_n))<<16) | BIT(R16_BOF(_n));
#ifdef CONFIG_PINCTRL_SPPCTL
		writel(r, pc->base1 + SPPCTL_GPIO_OFF_OD + R16_ROF(_n));
#else
		writel(r, pc->base0 + SPPCTL_GPIO_OFF_OD + R16_ROF(_n));
#endif
		break;

	case PIN_CONFIG_INPUT_ENABLE:
		KERR(_c->parent, "f_scf(%03d,%lX) input enable arg:%d\n", _n, _conf, ca);
		break;

	case PIN_CONFIG_OUTPUT:
		ret = sppctlgpio_f_sou(_c, _n, 0);
		break;

	case PIN_CONFIG_PERSIST_STATE:
		KDBG(_c->parent, "f_scf(%03d,%lX) not support pinconf:%d\n", _n, _conf, cp);
		ret = -ENOTSUPP;
		break;

	default:
		KDBG(_c->parent, "f_scf(%03d,%lX) unknown pinconf:%d\n", _n, _conf, cp);
		ret = -EINVAL;
		break;
	}

	return ret;
}

#ifdef CONFIG_DEBUG_FS
void sppctlgpio_f_dsh(struct seq_file *_s, struct gpio_chip *_c)
{
	int i;
	const char *label;

	for (i = 0; i < _c->ngpio; i++) {
		label = gpiochip_is_requested(_c, i);
		if (!label)
			label = "";

		seq_printf(_s, " gpio-%03d (%-16.16s | %-16.16s)", i + _c->base,
			   _c->names[i], label);
		seq_printf(_s, " %c", sppctlgpio_f_gdi(_c, i) == 0 ? 'O' : 'I');
		seq_printf(_s, ":%d", sppctlgpio_f_get(_c, i));
		seq_printf(_s, " %s", (sppctlgpio_u_gfrst(_c, i) ? "gpi" : "mux"));
		seq_printf(_s, " %s", (sppctlgpio_u_magpi(_c, i) ? "gpi" : "iop"));
		seq_printf(_s, " %s", (sppctlgpio_u_isinv(_c, i) ? "inv" : "   "));
		seq_printf(_s, " %s", (sppctlgpio_u_isodr(_c, i) ? "oDr" : ""));
		seq_puts(_s, "\n");
	}
}
#else
#define sppctlgpio_f_dsh NULL
#endif

int sppctlgpio_i_map(struct gpio_chip *_c, unsigned int _off)
{
	struct sppctlgpio_chip_t *pc = (struct sppctlgpio_chip_t *)gpiochip_get_data(_c);
	int i;

#if !defined(CONFIG_PINCTRL_SPPCTL)
	return -ENXIO;
#endif

	if ( _off < SPPCTL_MUXABLE_MIN || _off > SPPCTL_MUXABLE_MAX) {
	  KERR(_c->parent, "i_map: %d is not muxable\n", _off);
	  return -ENXIO;
	}

	for ( i = 0; i < SPPCTL_GPIO_IRQS; i++) {
	  if ( pc->irq[ i] < 0) continue;
	  if ( pc->irq_pin[ i] == _off) return pc->irq[ i];
	  if ( pc->irq_pin[ i] >= 0) continue;
	  sppctlgpio_u_magpi_set( _c, _off, muxF_M, muxMKEEP);
#ifdef SUPPORT_PINMUX
	  sppctl_pin_set( ( struct sppctl_pdata_t *)( _c->parent->platform_data), _off - 7, MUXF_GPIO_INT0 + i - 2);
#endif
	  pc->irq_pin[ i] = _off;
	  KDBG(_c->parent, "i_map: pin %d muxed to %d irq\n", _off, pc->irq[ i]);
	  return pc->irq[ i];
	}
	KERR(_c->parent, "i_map: no free IRQ for %d\n", _off);
	return -ENXIO;
}

void sppctlgpio_unmux_irq( struct gpio_chip *_c, unsigned _pin) {
	struct sppctlgpio_chip_t *pc = ( struct sppctlgpio_chip_t *)gpiochip_get_data( _c);
	int i;
	KDBG(_c->parent, "%s(%d)\n", __FUNCTION__, _pin);
	// if irq is binded - free it
	for ( i = 0; i < SPPCTL_GPIO_IRQS; i++) {
	  if ( pc->irq[ i] < 0) continue;
	  if ( pc->irq_pin[ i] != _pin) continue;
	  KDBG(_c->parent, "%s(%03d) detouching from irq: %d\n", __FUNCTION__, _pin, pc->irq[ i]);
#ifdef SUPPORT_PINMUX
	  sppctl_pin_set( ( struct sppctl_pdata_t *)( _c->parent->platform_data), 0, MUXF_GPIO_INT0 + i - 2);
#endif
	  pc->irq_pin[ i] = -1;
       }
}
