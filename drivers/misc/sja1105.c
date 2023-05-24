// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2017-2018, 2020-2021, 2023 NXP
 */

#include <common.h>
#include <dm/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <spi.h>
#include "sja1105_ll.h"

#ifndef CONFIG_DEFAULT_SPI_BUS
#   define CONFIG_DEFAULT_SPI_BUS	0
#endif

#define SJA_DSPI_MODE	(SPI_CPHA | SPI_FMSZ_16)
#define SJA_DSPI_HZ	4000000

#define sja_debug(fmt, ...) \
	debug("[SJA1105]%s:%d " fmt, __func__, __LINE__, ##__VA_ARGS__)

#ifdef CONFIG_DM_SPI
#define SPI_NAME_PRINT_TEMPLATE "generic_%d:%d"
#endif

static char *mac_lvl_counters1[] = {
	"N_RUNT         ",
	"N_SOFERR       ",
	"N_ALIGNERR     ",
	"N_MIIERR       ",
};

static char *mac_lvl_counters2[] = {
	"RSVD           ",
	"SPCERRS        ",
	"DRN664ERRS     ",
	"RSVD           ",
	"BAGDROP        ",
	"LENDROPS       ",
	"PORTDROPS      ",
	"RSVD           ",
	"SPCPRIOR       ",
	"RSVD           ",
};

static char *eth_high_lvl_counters1[] = {
	"N_TXBYTE       ",
	"N_TXBYTESH     ",
	"N_TXFRM        ",
	"N_TXFRMSH      ",
	"N_RXBYTE       ",
	"N_RXBYTESH     ",
	"N_RXFRM        ",
	"N_RXFRMSH      ",
	"N_POLERR       ",
	"RSVD           ",
	"RSVD           ",
	"N_CRCERR       ",
	"N_SIZERR       ",
	"RSVD           ",
	"N_VLANERR      ",
	"N_N664ERR      ",
};

static char *eth_high_lvl_counters2[] = {
	"N_NOT_REACH    ",
	"N_ERG_DISABLED ",
	"N_PART_DROP    ",
	"N_QFULL        ",
};

struct sja_parms {
	u32 bus;
	u32 cs;
	u32 devid;
	u32 bin_len;
	u8 *cfg_bin;
};

static struct spi_slave *get_spi_slave(struct sja_parms *sjap)
{
	struct spi_slave *slave;
#ifdef CONFIG_DM_SPI
#define MAX_ARRAY_SIZE 30
	int ret;
	char name[MAX_ARRAY_SIZE + 1];
	struct udevice *dev;

	name[MAX_ARRAY_SIZE] = 0;
	ret = snprintf(name, sizeof(name) - 1, SPI_NAME_PRINT_TEMPLATE,
		       sjap->bus, sjap->cs);
	if (ret > MAX_ARRAY_SIZE)
		return NULL;

	ret = spi_get_bus_and_cs(sjap->bus, sjap->cs, SJA_DSPI_HZ,
				 SJA_DSPI_MODE, "spi_generic_drv",
				 name, &dev, &slave);
	if (ret)
		return NULL;
	/*
	 * spi_get_bus_and_cs does not populate max_hz, mode and wordlen fields
	 * when there is a node in dts.
	 * In order to maintain the compatibility with the DM version of
	 * the driver, these fields should be assigned manually.
	 */
	slave->max_hz = SJA_DSPI_HZ;
	slave->mode = SJA_DSPI_MODE;
	slave->wordlen = SPI_DEFAULT_WORDLEN;
#else
	slave = spi_setup_slave(sjap->bus, sjap->cs, SJA_DSPI_HZ,
				SJA_DSPI_MODE);

#endif
	return slave;
}

static int sja1105_write(struct sja_parms *sjap, u32 *cmd, u8 nb_words)
{
	struct spi_slave *slave;
	int bitlen = (nb_words << 3) << 2;
	int ret = 0;

	slave = get_spi_slave(sjap);
	if (!slave) {
		printf("Invalid device %d:%d\n", sjap->bus, sjap->cs);
		return -EINVAL;
	}

	ret = spi_claim_bus(slave);
	if (ret) {
		printf("Error %d while claiming bus\n", ret);
		goto done;
	}

	ret = spi_xfer(slave, bitlen, cmd, NULL,
		       SPI_XFER_BEGIN | SPI_XFER_END);

	if (ret)
		printf("Error %d during SPI transaction\n", ret);

done:
	spi_release_bus(slave);
#ifndef CONFIG_DM_SPI
	spi_free_slave(slave);
#endif

	return ret;
}

static u32 swap_words16(u32 swap_val)
{
	u32 upper, lower;

	upper = (swap_val & GENMASK(15, 0)) << 16;
	lower = (swap_val & GENMASK(31, 16)) >> 16;
	return upper | lower;
}

static int sja1105_cfg_block_write(struct sja_parms *sjap, u32 reg_addr,
				   u32 *data, int nb_words)
{
	u32 cmd[SJA1105_CONFIG_WORDS_PER_BLOCK + 1] = { 0 };
	int i = 0;

	cmd[0] = cpu_to_le32(CMD_ENCODE_RWOP(CMD_WR_OP) |
			      CMD_ENCODE_ADDR(reg_addr));
	cmd[0] = swap_words16(cmd[0]);

	while (i < nb_words) {
		cmd[i + 1] = swap_words16(*data++);
		sja_debug("config write 0x%08x\n", cmd[i + 1]);
		i++;
	}

	return sja1105_write(sjap, cmd, nb_words + 1);
}

static u32 sja1105_read_reg32(struct sja_parms *sjap, u32 reg_addr)
{
	u32 cmd[2] = { 0 }, resp[2] = { 0 };
	struct spi_slave *slave;
	int bitlen = sizeof(cmd) << 3;
	int rc;

	sja_debug("reading 4bytes @0x%08x tlen %d t.bits_per_word %d\n",
		  reg_addr, 8, 64);

	slave = get_spi_slave(sjap);
	if (!slave) {
		printf("Invalid device %d:%d\n", sjap->bus, sjap->cs);
		return -EINVAL;
	}

	rc = spi_claim_bus(slave);
	if (rc)
		goto done;

	cmd[0] = cpu_to_le32(CMD_ENCODE_RWOP(CMD_RD_OP) |
		CMD_ENCODE_ADDR(reg_addr) | CMD_ENCODE_WRD_CNT(1));
	cmd[1] = 0;
	cmd[0] = swap_words16(cmd[0]);

	rc = spi_xfer(slave, bitlen, cmd, resp,
		      SPI_XFER_BEGIN | SPI_XFER_END);
	if (rc)
		printf("Error %d during SPI transaction\n", rc);
	spi_release_bus(slave);
#ifndef CONFIG_DM_SPI
	spi_free_slave(slave);
#endif

	resp[1] = swap_words16(resp[1]);

	return le32_to_cpu(resp[1]);
done:
	return rc;
}

static int sja1105_write_reg32(struct sja_parms *sjap, u32 reg_addr, u32 val)
{
	u32 cmd[2] = { 0 }, resp[2] = { 0 };
	struct spi_slave *slave;
	int bitlen = sizeof(cmd) << 3;
	int rc;

	sja_debug("writing 4bytes @0x%08x tlen %d t.bits_per_word %d\n",
		  reg_addr, 8, 64);

	slave = get_spi_slave(sjap);
	if (!slave) {
		printf("Invalid device %d:%d\n", sjap->bus, sjap->cs);
		return -EINVAL;
	}

	rc = spi_claim_bus(slave);
	if (rc)
		goto done;

	cmd[0] = cpu_to_le32(CMD_ENCODE_RWOP(CMD_WR_OP) |
		CMD_ENCODE_ADDR(reg_addr) | CMD_ENCODE_WRD_CNT(1));
	cmd[0] = swap_words16(cmd[0]);
	cmd[1] = swap_words16(val);

	rc = spi_xfer(slave, bitlen, cmd, resp,
		      SPI_XFER_BEGIN | SPI_XFER_END);
	if (rc)
		printf("Error %d during SPI transaction\n", rc);
	spi_release_bus(slave);
#ifndef CONFIG_DM_SPI
	spi_free_slave(slave);
#endif

	resp[1] = swap_words16(resp[1]);

	return le32_to_cpu(resp[1]);
done:
	return rc;
}

static bool sja1105_check_device_status(struct sja_parms *sjap,
					bool expected_status, bool *pstatus)
{
	u32 status;
	u32 expected_val = expected_status ? SJA1105_BIT_STATUS_CONFIG_DONE : 0;
	bool ret = true;
	u32 error;

	status = sja1105_read_reg32(sjap, SJA1105_REG_STATUS);

	/* Check status is valid: check if any error bit is set */
	error = SJA1105_BIT_STATUS_CRCCHKL |
		 SJA1105_BIT_STATUS_DEVID_MATCH |
		 SJA1105_BIT_STATUS_CRCCHKG;
	if (status & error) {
		sja_debug("Error: SJA1105_REG_STATUS=0x%08x - LocalCRCfail=%d - DevID unmatched=%d, GlobalCRCfail=%d\n",
			  status,
			  (int)(status & SJA1105_BIT_STATUS_CRCCHKL),
			  (int)(status & SJA1105_BIT_STATUS_DEVID_MATCH),
			  (int)(status & SJA1105_BIT_STATUS_CRCCHKG));
		return false;
	}

	*pstatus = (expected_val == (status & SJA1105_BIT_STATUS_CONFIG_DONE));

	if (expected_status && !*pstatus)
		ret = false;

	return ret;
}

static int sja1105_check_device_id(struct sja_parms *sjap)
{
	return sja1105_read_reg32(sjap, SJA1105_REG_DEVICEID);
}

bool sja1105_post_cfg_load_check(struct sja_parms *sjap)
{
	u32 chip_id;
	bool status;

	/* Trying to read back the SJA1105 status via SPI... */
	chip_id  = sja1105_check_device_id(sjap);
	if (sjap->devid != chip_id)
		return false;
	if (!sja1105_check_device_status(sjap, true, &status))
		return false;

	return status;
}

static void sja1105_en_rgmii_txid_by_default(struct sja_parms *sjap, int port)
{
	u32 id;
	u32 reg = SJA1105_CFG_PAD_MIIX_ID_PORT(port);

	id = sja1105_read_reg32(sjap, reg);

	if (id & (SJA1105_CFG_PAD_MIIX_ID_TXC_PD |
				SJA1105_CFG_PAD_MIIX_ID_TXC_BYPASS)) {
		id &= ~GENMASK(7, 0);
		id |= SJA1105_CFG_PAD_MIIX_ID_RGMII_TXID;
		sja1105_write_reg32(sjap, reg, id);
	}
}

static void sja1105_set_rgmii_clock(struct sja_parms *sjap, int port, int speed)
{
	u32 divisor = 0U;

	/* Set slew rate of TX Pins to high speed */
	sja1105_write_reg32(sjap,
			    SJA1105_CFG_PAD_MIIX_TX_PORT(port),
			    SJA1105_CFG_PAD_MIIX_TX_SLEW_RGMII);

	/* Set IDIV divisor (IDIV = divisor + 1) */
	if (speed == SJA1105_REG_MAC_SPEED_10M)
		divisor = 9U;
	else if (speed == SJA1105_REG_MAC_SPEED_100M)
		divisor = 0U;

	switch (speed) {
	case SJA1105_REG_MAC_SPEED_1G:
	case SJA1105_REG_MAC_SPEED_DISABLED:
		/* Set Clock delay */
		sja1105_en_rgmii_txid_by_default(sjap, port);
		/* Disable IDIV */
		sja1105_write_reg32(sjap, SJA1105_CGU_IDIV_PORT(port),
				    SJA1105_CGU_IDIV_DISABLE);

		/* Set Clock source to PLL0 125MHz */
		sja1105_write_reg32(sjap,
				    SJA1105_CGU_MII_TX_CLK_PORT(port),
				    SJA1105_CGU_MII_CLK_SRC_PLL0);
		break;
	case SJA1105_REG_MAC_SPEED_100M:
	case SJA1105_REG_MAC_SPEED_10M:
		/* Set Clock delay */
		sja1105_write_reg32(sjap,
				    SJA1105_CFG_PAD_MIIX_ID_PORT(port),
				    SJA1105_CFG_PAD_MIIX_ID_RGMII_SLOW);

		/* Enable IDIV with divisor */
		sja1105_write_reg32(sjap, SJA1105_CGU_IDIV_PORT(port),
				    SJA1105_CGU_IDIV_ENABLE |
				    ENCODE_REG_IDIV_IDIV(divisor));

		/* Set Clock source to IDIV (25Mhz XTAL / (divisor + 1))*/
		sja1105_write_reg32(sjap,
				    SJA1105_CGU_MII_TX_CLK_PORT(port),
				    SJA1105_CGU_MII_SRC_IDIV(port));
		break;
	default:
		pr_err("speed not supported");
	}
}

void sja1105_port_cfg(struct sja_parms *sjap)
{
	u32 i;

	for (i = 0; i < SJA1105_PORT_NB; i++) {
		u32 port_status;

		/* Get port type / speed */
		port_status = sja1105_read_reg32(sjap,
						 SJA1105_PORT_STATUS_MII_PORT(i));

		switch (port_status & SJA1105_PORT_STATUS_MII_MODE) {
		case e_mii_mode_rgmii:
			/* 1G sets the port to default configuration */
			sja1105_set_rgmii_clock(sjap, i,
						SJA1105_REG_MAC_SPEED_1G);
			break;

		default:
			break;
		}
	}
}

static void sja1105_set_speed_reg(struct sja_parms *sjap, int port, u32 speed)
{
	u32 reg_val, mode;

	/* Get port type / speed */
	mode = sja1105_read_reg32(sjap,
				  SJA1105_PORT_STATUS_MII_PORT(port));

	switch (mode & SJA1105_PORT_STATUS_MII_MODE) {
	case e_mii_mode_rgmii:
		/* Swap to configuration for required port*/
		sja1105_write_reg32(sjap, SJA1105_REG_MAC_RECONF0,
				    SJA1105_BIT_MAC_RECONF0_PORT(port) |
				    SJA1105_BIT_MAC_RECONF0_VALID);

		/* Read current content of register and update speed */
		reg_val = sja1105_read_reg32(sjap, SJA1105_REG_MAC_RECONF5);
		reg_val &= ~SJA1105_REG_MAC_SPEED_MASK;
		reg_val |= SJA1105_REG_MAC_RECONF5_SPEED(speed);
		sja1105_write_reg32(sjap, SJA1105_REG_MAC_RECONF5, reg_val);

		/* Write configuration back */
		sja1105_write_reg32(sjap,
				    SJA1105_REG_MAC_RECONF0,
				    SJA1105_BIT_MAC_RECONF0_WRITE |
				    SJA1105_BIT_MAC_RECONF0_PORT(port) |
				    SJA1105_BIT_MAC_RECONF0_VALID);

		/* Check error */
		reg_val = sja1105_read_reg32(sjap, SJA1105_REG_MAC_RECONF0);
		if (reg_val & SJA1105_BIT_MAC_RECONF0_ERR) {
			pr_err("speed on port %d could't be updated", port);
			return;
		}

		/* Update Clock Generation Unit registers */
		sja1105_set_rgmii_clock(sjap, port, speed);
		break;
	default:
		pr_err("only RGMII is supported");
		break;
	}
}

static u32 sja1105_get_speed_reg(struct sja_parms *sjap, int port)
{
	u32 reg_val;
	/* Swap to configuration for required port*/
	sja1105_write_reg32(sjap, SJA1105_REG_MAC_RECONF0,
			    SJA1105_BIT_MAC_RECONF0_PORT(port) |
			    SJA1105_BIT_MAC_RECONF0_VALID);

	/* Read current content of register and update speed */
	reg_val = sja1105_read_reg32(sjap, SJA1105_REG_MAC_RECONF5);

	return ((SJA1105_REG_MAC_SPEED_MASK & reg_val) >>
		SJA1105_REG_MAC_SPEED_SHIFT);
}

static bool sja1105_parse_speed(char *str, u32 *speed)
{
	if (!strcmp(str, "10M"))
		*speed = SJA1105_REG_MAC_SPEED_10M;
	else if (!strcmp(str, "100M"))
		*speed = SJA1105_REG_MAC_SPEED_100M;
	else if (!strcmp(str, "1G"))
		*speed = SJA1105_REG_MAC_SPEED_1G;
	else if (!strcmp(str, "disable"))
		*speed = SJA1105_REG_MAC_SPEED_DISABLED;
	else if (!strcmp(str, "-"))
		*speed = -1;
	else
		return false;
	return true;
}

static int sja1105_configuration_load(struct sja_parms *sjap)
{
	int remaining_words;
	int nb_words;
	u32 *data;
	u32 dev_addr;
	u32 val;
	bool swap_required;
	int i;

	if (!sjap->cfg_bin) {
		printf("Error: SJA1105 Switch configuration is NULL\n");
		return -EINVAL;
	}

	if (sjap->bin_len == 0) {
		printf("Error: SJA1105 Switch configuration is empty\n");
		return -EINVAL;
	}

	if (sjap->bin_len % 4 != 0) {
		printf("Error: SJA1105 Switch configuration is not valid\n");
		return -EINVAL;
	}

	data = (u32 *)&sjap->cfg_bin[0];

	nb_words = (sjap->bin_len >> 2);

	val = data[0];

	if (val == __builtin_bswap32(sjap->devid)) {
		printf("Config bin requires swap, incorrect endianness\n");
		swap_required = true;
	} else if (val == sjap->devid) {
		swap_required = false;
	} else {
		printf("Error: SJA1105 unhandled revision Switch incompatible configuration file (%x - %x)\n",
		       val, sjap->devid);
		return -EINVAL;
	}

	if (swap_required)
		for (i = 0; i < nb_words; i++) {
			val = data[i];
			data[i] = __builtin_bswap32(val);
		}

	sja_debug("swap_required %d nb_words %d dev_addr 0x%08x\n",
		  swap_required, nb_words, (u32)SJA1105_CONFIG_START_ADDRESS);

	remaining_words = nb_words;
	dev_addr = SJA1105_CONFIG_START_ADDRESS;

	i = 0;
	while (remaining_words > 0) {
		int block_size_words =
			min(SJA1105_CONFIG_WORDS_PER_BLOCK, remaining_words);

		sja_debug("block_size_words %d remaining_words %d\n",
			  block_size_words, remaining_words);

		if (sja1105_cfg_block_write(sjap, dev_addr, data,
					    block_size_words) < 0)
			return 1;

		sja_debug("Loaded block %d @0x%08x\n", i, dev_addr);

		dev_addr += block_size_words;
		data += block_size_words;
		remaining_words -= block_size_words;
		i++;

		if (i % 10 == 0)
			sja1105_post_cfg_load_check(sjap);
	}

	if (!sja1105_post_cfg_load_check(sjap)) {
		printf("SJA1105 configuration failed\n");
		return -ENXIO;
	}

	sja1105_port_cfg(sjap);

	return 0;
}

static bool sja1105_speed_control(struct sja_parms *sjap, char *options)
{
	int port;
	u32 speed[SJA1105_PORT_NB] = {-1, -1, -1, -1, -1};
	char *tok;
	static const char * const speed_str[] = {"Disabled",
						 "1G",
						 "100M",
						 "10M"};

	switch (sja1105_check_device_id(sjap)) {
	case SJA1105_DEV_COMPATIBLE_PRx:
	case SJA1105_DEV_COMPATIBLE_QSx:
		break;
	default:
		pr_err("command not supported on this device");
		return false;
	}

	if (options) {
		/* Additional parameter to set speed */
		tok = strtok(options, ",");
		for (port = 0; port < SJA1105_PORT_NB; ++port) {
			if (!tok)
				break;

			if (!sja1105_parse_speed(tok, &speed[port])) {
				pr_err("invalid speed on port %d", port);
				return false;
			}

			tok = strtok(NULL, ",");
		}

		/* Update registers */
		for (port = 0; port < SJA1105_PORT_NB; ++port)
			if (-1 != speed[port])
				sja1105_set_speed_reg(sjap, port, speed[port]);
	}

	for (port = 0; port < SJA1105_PORT_NB; ++port)
		speed[port] = sja1105_get_speed_reg(sjap, port);

	printf("port0:%s port1:%s port2:%s port3:%s port4:%s\n",
	       speed_str[speed[0]], speed_str[speed[1]], speed_str[speed[2]],
	       speed_str[speed[3]], speed_str[speed[4]]);

	return true;
}

void sja1105_reset_ports(u32 cs, u32 bus)
{
	struct sja_parms sjap;
	int i, val;

	sjap.cs = cs;
	sjap.bus = bus;

	for (i = 0; i < SJA1105_PORT_NB; i++) {
		val = sja1105_read_reg32(&sjap,
					 SJA1105_CFG_PAD_MIIX_ID_PORT(i));

		/* If the RXID is disabled, skip the port */
		if (val & SJA1105_CFG_PAD_MIIX_ID_RXC_PD ||
		    val & SJA1105_CFG_PAD_MIIX_ID_RXC_BYPASS)
			continue;

		/* Toggle RX Internal delay PowerDown and Bypass */
		val |= SJA1105_CFG_PAD_MIIX_ID_RXC_PD;
		val |= SJA1105_CFG_PAD_MIIX_ID_RXC_BYPASS;

		sja1105_write_reg32(&sjap, SJA1105_CFG_PAD_MIIX_ID_PORT(i),
				    val);

		val &= ~SJA1105_CFG_PAD_MIIX_ID_RXC_PD;
		val &= ~SJA1105_CFG_PAD_MIIX_ID_RXC_BYPASS;

		sja1105_write_reg32(&sjap, SJA1105_CFG_PAD_MIIX_ID_PORT(i),
				    val);
	}
}

int sja1105_probe(u32 cs, u32 bus)
{
	struct sja_parms sjap;
	int ret = 0;

	memset(&sjap, 0, sizeof(struct sja_parms));

	sjap.cs = cs;
	sjap.bus = bus;

	sjap.devid = sja1105_check_device_id(&sjap);

	sja_debug("devid %X\n", sjap.devid);

	if (sja1105_post_cfg_load_check(&sjap)) {
		sja_debug("SJA1105 configuration already done. Skipping switch configuration\n");
		return 0;
	}

	printf("Loading SJA1105 firmware over SPI %d:%d\n", bus, cs);

	ret = sja1105_get_cfg(sjap.devid, sjap.cs, &sjap.bin_len,
			      &sjap.cfg_bin);

	if (ret) {
		printf("Error SJA1105 configuration not completed\n");
		return -EINVAL;
	}

	return sja1105_configuration_load(&sjap);
}

static int sja11105_dm_probe(struct udevice *dev)
{
#ifdef CONFIG_DM_SPI
	int cs = spi_chip_select(dev);
	int bus = dev->parent->seq;
#else
	struct spi_slave *slave = dev_get_parent_priv(dev);
	int cs = slave->cs;
	int bus = slave->bus;
#endif

	if (cs < 0)
		return cs;

	return sja1105_probe(cs, bus);
}

static void sja1105_print_general_status_regs(struct sja_parms *sjap)
{
	u32 val32;
	int i, sja1105_gen_reg_status_nr, offset = -1;

	if (!sjap)
		return -EINVAL;

	printf("\nGeneral Status\n");
	for (i = SJA1105_REG_GENERAL_STATUS1; i < SJA1105_REG_GENERAL_STATUS9; i++) {
		if (i == SJA1105_REG_GENERAL_STATUS_NA) {
			offset++;
			continue;
		}

		val32 = sja1105_read_reg32(sjap, i);
		sja1105_gen_reg_status_nr = i - SJA1105_REG_GENERAL_STATUS1 - offset;
		printf("general_status_%d    = %08x\n", sja1105_gen_reg_status_nr, val32);
	}
}

static void sja1105_print_char_arr(struct sja_parms *sjap,
				   char *buffer[], int size, int port)
{
	int i;
	u32 val32, mask, shr_value = -1U;
	u32 reg_read_value;
	bool read_again, shr_needed;

	if (buffer == mac_lvl_counters1) {
		reg_read_value = SJA1105_REG_PORT_MAC_STATUS(port);
		read_again = false;
		mask = GENMASK(7, 0);
		shr_needed = true;
		shr_value = 8;
	}

	if (buffer == mac_lvl_counters2) {
		reg_read_value = SJA1105_REG_PORT_MAC_STATUS(port) + 1;
		read_again = false;
		mask = GENMASK(1, 0);
		shr_needed = true;
		shr_value = 1;
	}

	if (buffer == eth_high_lvl_counters1) {
		reg_read_value = SJA1105_REG_PORT_HIGH_STATUS1(port);
		read_again = true;
		mask = GENMASK(31, 0);
		shr_needed = false;
		shr_value = 0;
	}

	if (buffer == eth_high_lvl_counters2) {
		reg_read_value = SJA1105_REG_PORT_HIGH_STATUS2(port);
		read_again = true;
		mask = GENMASK(31, 0);
		shr_needed = false;
		shr_value = 0;
	}

	if (shr_value == -1U)
		return;

	val32 = sja1105_read_reg32(sjap, reg_read_value);
	for (i = 0; i < size; i++) {
		printf("port%d %s    = %u\n", port, buffer[i],
		       (val32 >> (i * shr_needed * shr_value)) & mask);

		if (read_again && i + 1 < size)
			val32 = sja1105_read_reg32(sjap, reg_read_value + i + 1);
	}
}

static void sja1105_print_level_counters(struct sja_parms *sjap)
{
	int i;

	for (i = 0; i < SJA1105_PORT_NB; i++) {
		printf("\nEthernet MAC-level status port%d\n", i);
		sja1105_print_char_arr(sjap, mac_lvl_counters1,
				       ARRAY_SIZE(mac_lvl_counters1), i);
		sja1105_print_char_arr(sjap, mac_lvl_counters2,
				       ARRAY_SIZE(mac_lvl_counters2), i);
	}

	for (i = 0; i < SJA1105_PORT_NB; i++) {
		printf("\nEthernet High-level status port%d\n", i);
		sja1105_print_char_arr(sjap, eth_high_lvl_counters1,
				       ARRAY_SIZE(eth_high_lvl_counters1), i);
		sja1105_print_char_arr(sjap, eth_high_lvl_counters2,
				       ARRAY_SIZE(eth_high_lvl_counters2), i);
	}
}

static void sja1105_print_internal_delay_status(struct sja_parms *sjap)
{
	u32 val32;
	int i;

	printf("\nInternal delay status\n");

	for (i = 0; i < SJA1105_PORT_NB; i++) {
		val32 = sja1105_read_reg32(sjap,
					   SJA1105_CFG_PAD_MIIX_ID_PORT(i));
		printf("CFG_PAD_MII%d_ID 0x%x\n", i, val32);
	}
}

static int sja1105_print_info(struct sja_parms *sjap)
{
	if (!sjap)
		return -EINVAL;

	sja1105_print_general_status_regs(sjap);
	sja1105_print_level_counters(sjap);
	sja1105_print_internal_delay_status(sjap);
	return 0;
}

static int do_sja_cmd(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	char  *cp = NULL, *options = NULL;
	struct sja_parms sjap;

	/* Parse SPI data */
	sjap.cs = 0;
	sjap.bus = CONFIG_DEFAULT_SPI_BUS;

	if (argc < 2)
		return CMD_RET_USAGE;

	/* Check if last argument is spi:cs */
	if (argc > 2 && !strchr(argv[argc - 1], ',')) {
		sjap.bus = simple_strtoul(argv[argc - 1], &cp, 10);
		if (*cp == ':') {
			sjap.cs = simple_strtoul(cp + 1, &cp, 10);
		} else {
			sjap.cs = sjap.bus;
			sjap.bus = CONFIG_DEFAULT_SPI_BUS;
		}
	}

	if (!strcmp(argv[1], "probe")) {
		printf("Probe SJA1105\n");
		/* For debug purposes force SJA1105 initialization*/
		sja1105_probe(sjap.cs, sjap.bus);
		sja1105_reset_ports(sjap.cs, sjap.bus);
		/* end of force SJA1105 initialization*/
	} else if (!strcmp(argv[1], "info")) {
		sja1105_print_info(&sjap);
	} else if (!strcmp(argv[1], "speed")) {
		if (argc >= 3 && !strchr(argv[2], ':'))
			options = argv[2];
		if (!sja1105_speed_control(&sjap, options))
			return CMD_RET_USAGE;
	} else {
		return CMD_RET_USAGE;
	}

	return 0;
}

U_BOOT_CMD(
	sja,	4,	1,	do_sja_cmd,
	"SJA1105 control",
	"sja probe [<bus>:]<cs> - Probe SJA and load configuration\n"
	"sja info [<bus>:]<cs> - View registers for SJA\n"
	"sja speed [<bus>:]<cs> - Read configured speed on all ports\n"
	"sja speed [p0speed,p1speed,p2speed,p3speed,p3speed] [<bus>:]<cs> - Set speed\n"
	"          for ports, speed options [-|disable|10M|100M|1G] when \"-\" is set\n"
	"          given port is not updated\n"
);

static const struct udevice_id sja1105_ids[] = {
	{ .compatible = "nxp,sja1105-fw-loader" },
	{}
};

U_BOOT_DRIVER(altera_sysid) = {
	.name	= "sja1105_fw_loader",
	.id	= UCLASS_MISC,
	.of_match = sja1105_ids,
	.probe	= sja11105_dm_probe,
};
