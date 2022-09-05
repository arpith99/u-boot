// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2021-2022 NXP
 */

#include <common.h>
#include <dm.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <dm/pinctrl.h>

#include <linux/compat.h>
#include <linux/list.h>

#include <malloc.h>

#define S32_PAD(pin)	((pin) * 4)

#define SIUL2_MSCR_IBE	BIT(19)
#define SIUL2_MSCR_OBE	BIT(21)
#define SIUL2_MSCR_ODE	BIT(20)

#define SIUL2_MSCR_SSS_MASK	0x7

#define UPTR(a) ((uintptr_t)(a))

#define SIUL2_NXP_PINS "nxp,pins"

struct s32_range {
	void __iomem *base_addr;
	u32 begin;
	u32 end;
};

struct s32_pin {
	u32 pin;
	u32 config;
	struct list_head list;
};

struct s32_pinctrl {
	struct s32_range *ranges;
	int num_ranges;
	struct list_head gpio_configs;
};

static struct s32_range *s32_get_pin_range(struct s32_pinctrl *ctlr, u32 pin)
{
	int i;
	struct s32_range *range = NULL;

	for (i = 0; i < ctlr->num_ranges; ++i) {
		range = &ctlr->ranges[i];
		if (pin >= range->begin && pin <= range->end)
			return range;
	}

	return NULL;
}

static int s32_set_state(struct udevice *dev, struct udevice *config)
{
	struct s32_pinctrl *priv = dev_get_priv(dev);
	u32 pin, function;
	struct s32_range *range = NULL;
	int index = 0;
	int sz;
	int ret;

	if (!dev_read_prop(config, "fsl,pins", &sz)) {
		pr_err("fsl,pins property not found\n");
		return -EINVAL;
	}

	sz >>= 2;
	if (sz % 2) {
		pr_err("fsl,pins invalid array size: %d\n", sz);
		return -EINVAL;
	}

	while (index < sz) {
		ret = dev_read_u32_index(config, "fsl,pins", index++, &pin);
		if (ret) {
			pr_err("failed to read pin ID\n");
			return ret;
		}
		ret = dev_read_u32_index(config, "fsl,pins", index++,
					 &function);
		if (ret) {
			pr_err("failed to read pin function\n");
			return ret;
		}

		range = s32_get_pin_range(priv, pin);
		if (range) {
			pin -= range->begin;
			writel(function, UPTR(range->base_addr) + S32_PAD(pin));
			pr_debug("%s function:reg 0x%x:0x%p\n", config->name,
				 function, range->base_addr + S32_PAD(pin));
			continue;
		}

		pr_err("%s invalid pin found function:pin 0x%x:%d\n",
		       config->name, function, pin);

		return -ENOENT;
	};

	return 0;
}

static int s32_pinmux_set(struct udevice *dev, unsigned int pin_selector,
			  unsigned int func_selector)
{
	struct s32_pinctrl *priv = dev_get_priv(dev);
	struct s32_range *range = s32_get_pin_range(priv, pin_selector);
	u32 mscr_value;

	if (!range)
		return -ENOENT;

	pin_selector -= range->begin;
	mscr_value = readl(UPTR(range->base_addr) + S32_PAD(pin_selector));
	mscr_value &= ~SIUL2_MSCR_SSS_MASK;
	mscr_value |= (func_selector & SIUL2_MSCR_SSS_MASK);

	writel(mscr_value, UPTR(range->base_addr) + S32_PAD(pin_selector));

	return 0;
}

static int s32_pinconf_set(struct udevice *dev, unsigned int pin_selector,
			   unsigned int param, unsigned int argument)
{
	struct s32_pinctrl *priv = dev_get_priv(dev);
	struct s32_range *range = s32_get_pin_range(priv, pin_selector);
	enum pin_config_param cfg = (enum pin_config_param)param;
	u32 mscr_value;

	if (!range)
		return -ENOENT;

	pin_selector -= range->begin;
	mscr_value = readl(UPTR(range->base_addr) + S32_PAD(pin_selector));

	switch (cfg) {
	case PIN_CONFIG_OUTPUT_ENABLE:
		mscr_value &= ~SIUL2_MSCR_OBE;
		if (argument)
			mscr_value |= SIUL2_MSCR_OBE;
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		mscr_value &= ~SIUL2_MSCR_IBE;
		if (argument)
			mscr_value |= SIUL2_MSCR_IBE;
		break;
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		mscr_value |= SIUL2_MSCR_ODE;
		break;
	case PIN_CONFIG_DRIVE_PUSH_PULL:
		mscr_value &= ~SIUL2_MSCR_ODE;
		break;
	default:
		return -ENOSYS;
	}

	writel(mscr_value, UPTR(range->base_addr) + S32_PAD(pin_selector));

	return 0;
}

static int s32_gpio_request_enable(struct udevice *dev, unsigned int pin_selector)
{
	struct s32_pinctrl *priv = dev_get_priv(dev);
	struct s32_range *range = s32_get_pin_range(priv, pin_selector);
	u32 mscr_value;
	struct s32_pin *pin;

	if (!range)
		return -ENOENT;

	pin = malloc(sizeof(*pin));
	if (!pin)
		return -ENOMEM;

	pin->pin = pin_selector;

	pin_selector -= range->begin;
	mscr_value = readl(UPTR(range->base_addr) + S32_PAD(pin_selector));

	pin->config = mscr_value;

	mscr_value &= ~SIUL2_MSCR_SSS_MASK;

	writel(mscr_value, UPTR(range->base_addr) + S32_PAD(pin_selector));

	list_add(&pin->list, &priv->gpio_configs);

	return 0;
}

static int s32_gpio_disable_free(struct udevice *dev, unsigned int pin_selector)
{
	struct s32_pinctrl *priv = dev_get_priv(dev);
	struct s32_range *range = s32_get_pin_range(priv, pin_selector);
	struct list_head *pos, *temp;

	if (!range)
		return -ENOENT;

	list_for_each_safe(pos, temp, &priv->gpio_configs) {
		struct s32_pin *pin = list_entry(pos, struct s32_pin, list);

		if (pin->pin == pin_selector) {
			pin_selector -= range->begin;

			writel(pin->config,
			       UPTR(range->base_addr) + S32_PAD(pin_selector));

			list_del(pos);
			kfree(pin);
			break;
		}
	}

	return 0;
}

static int s32_get_gpio_mux(struct udevice *dev, __maybe_unused int banknum,
			    int index)
{
	struct s32_pinctrl *priv = dev_get_priv(dev);
	struct s32_range *range = s32_get_pin_range(priv, index);
	u32 mscr_value;
	u32 sss_value;

	if (!range)
		return -ENOENT;

	index -= range->begin;
	mscr_value = readl(UPTR(range->base_addr) + S32_PAD(index));

	sss_value = mscr_value & SIUL2_MSCR_SSS_MASK;

	if (sss_value != 0)
		return GPIOF_FUNC;

	if (mscr_value & SIUL2_MSCR_OBE)
		return GPIOF_OUTPUT;
	else if (mscr_value & SIUL2_MSCR_IBE)
		return GPIOF_INPUT;

	return GPIOF_UNKNOWN;
}

static const struct pinctrl_ops s32_pinctrl_ops = {
	.set_state		= s32_set_state,
	.gpio_request_enable	= s32_gpio_request_enable,
	.gpio_disable_free	= s32_gpio_disable_free,
	.pinmux_set		= s32_pinmux_set,
	.pinconf_set		= s32_pinconf_set,
	.get_gpio_mux		= s32_get_gpio_mux,
};

static int s32_pinctrl_probe(struct udevice *dev)
{
	struct s32_pinctrl *priv = dev_get_priv(dev);
	int ret, i, num_ranges;
	const void *pins_prop;
	fdt_addr_t addr;
	u32 begin, end;

	pins_prop = dev_read_prop(dev, SIUL2_NXP_PINS, &num_ranges);
	if (!pins_prop || (num_ranges % sizeof(u32) != 0)) {
		dev_err(dev, "Error retrieving %s!\n", SIUL2_NXP_PINS);
		return -EINVAL;
	}

	/* For each range we have a start and an end. */
	num_ranges /= (sizeof(u32) * 2);
	priv->num_ranges = num_ranges;

	priv->ranges = malloc(num_ranges * sizeof(*priv->ranges));
	if (!priv->ranges)
		return -ENOMEM;

	for (i = 0; i < num_ranges; ++i) {
		addr = dev_read_addr_index(dev, i);
		if (addr == FDT_ADDR_T_NONE) {
			dev_err(dev, "Error retrieving reg: %d\n", i);
			return -EINVAL;
		}

		priv->ranges[i].base_addr = (__iomem void *)addr;

		ret = dev_read_u32_index(dev, SIUL2_NXP_PINS, i * 2, &begin);
		if (ret) {
			dev_err(dev, "Error reading %s start: %d\n",
				SIUL2_NXP_PINS,
				ret);
			return ret;
		}

		ret = dev_read_u32_index(dev, SIUL2_NXP_PINS, i * 2 + 1, &end);
		if (ret) {
			dev_err(dev, "Error reading %s no. GPIOs: %d\n",
				SIUL2_NXP_PINS,
				ret);
			return ret;
		}

		priv->ranges[i].begin = begin;
		priv->ranges[i].end = end;
	}

	INIT_LIST_HEAD(&priv->gpio_configs);

	return 0;
}

static const struct udevice_id s32_pinctrl_ids[] = {
	{ .compatible = "nxp,s32g-siul2-pinctrl" },
	{ .compatible = "nxp,s32r45-siul2-pinctrl" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(s32_pinctrl) = {
	.name = "s32_pinctrl",
	.id = UCLASS_PINCTRL,
	.of_match = of_match_ptr(s32_pinctrl_ids),
	.probe = s32_pinctrl_probe,
	.priv_auto = sizeof(struct s32_pinctrl),
	.ops = &s32_pinctrl_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
