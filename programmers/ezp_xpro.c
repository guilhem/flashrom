/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdlib.h>
#include <libusb.h>
#include "platform/string.h"
#include "flash.h"
#include "flashchips.h"
#include "programmer.h"
#include "helpers.h"
#include "log.h"
#include "platform/udelay.h"

#define EZP_COMMAND_EP 0x02
#define EZP_DATA_EP 0x01
#define EZP_READ_EP 0x83
#define EZP_TIMEOUT 1000
#define EZP_PACKET_SIZE 20
#define EZP_BLOCK_SIZE 4096

struct ezp_xpro_data {
	libusb_context *usb;
	libusb_device_handle *handle;
	const struct flashchip *chip;
	uint8_t packet[EZP_PACKET_SIZE];
	bool claimed;
	bool active;
	bool identified;
	bool failed;
	uint8_t firmware;
};

static const struct dev_entry devs_ezp_xpro[] = {
	{0x0716, 0x5018, OK, "GZU-tek", "EZP XPro"},
	{0},
};

static void ezp_put_be32(uint8_t *buf, unsigned int value)
{
	buf[0] = value >> 24;
	buf[1] = value >> 16;
	buf[2] = value >> 8;
	buf[3] = value;
}

static int ezp_transfer(struct ezp_xpro_data *data, uint8_t endpoint,
			uint8_t *buf, unsigned int len, unsigned int timeout)
{
	int transferred = 0;
	int ret = libusb_bulk_transfer(data->handle, endpoint, buf, len, &transferred, timeout);
	if (ret < 0 || transferred < 0 || (unsigned int)transferred != len) {
		msg_perr("EZP XPro transfer on endpoint 0x%02x failed: %s (%d/%u bytes).\n",
			 endpoint, libusb_error_name(ret), transferred, len);
		data->failed = true;
		return 1;
	}
	return 0;
}

static int ezp_command(struct ezp_xpro_data *data, uint8_t command,
			unsigned int argument)
{
	uint8_t packet[EZP_PACKET_SIZE];
	memcpy(packet, data->packet, sizeof(packet));
	packet[1] = (packet[1] & 0xc0) | command;
	ezp_put_be32(packet + 13, argument);
	return ezp_transfer(data, EZP_COMMAND_EP, packet, sizeof(packet), EZP_TIMEOUT);
}

static int ezp_shutdown(void *context)
{
	struct ezp_xpro_data *data = context;
	int ret = 0;
	if (data->active)
		ret = ezp_command(data, 0x09, 0);
	if (data->claimed && libusb_release_interface(data->handle, 0) < 0)
		ret = 1;
	if (data->handle)
		libusb_close(data->handle);
	libusb_exit(data->usb);
	free(data);
	return ret;
}

static bool ezp_range_valid(const struct ezp_xpro_data *data,
			  unsigned int start, unsigned int len)
{
	unsigned int size = data->chip->total_size * KiB;
	return data->identified && !data->failed && start <= size && len <= size - start;
}

static int ezp_probe(struct flashctx *flash)
{
	struct ezp_xpro_data *data = flash->mst->opaque.data;
	uint8_t id[3];
	data->identified = false;
	if (data->failed)
		return 0;
	data->active = true;
	if (ezp_command(data, 0x00, 0))
		return 0;
	default_delay(30 * 1000);
	if (ezp_command(data, 0x07, 0) ||
	    ezp_transfer(data, EZP_READ_EP, id, sizeof(id), EZP_TIMEOUT))
		return 0;
	msg_pinfo("EZP XPro JEDEC ID: %02x %02x %02x\n", id[0], id[1], id[2]);
	if (id[0] != data->chip->manufacture_id ||
	    (((unsigned int)id[1] << 8) | id[2]) != data->chip->model_id) {
		msg_perr("JEDEC ID does not match selected chip %s.\n", data->chip->name);
		return 0;
	}
	data->identified = true;
	flash->chip->vendor = data->chip->vendor;
	flash->chip->name = data->chip->name;
	flash->chip->total_size = data->chip->total_size;
	flash->chip->page_size = data->chip->page_size;
	flash->chip->voltage = data->chip->voltage;
	flash->chip->tested = data->firmware == 0x03 ? TEST_OK_PREW : TEST_OK_PROBE;
	flash->chip->tested.wp = NA;
	/* Batch changed bytes into pages to avoid repeated whole-image verification. */
	flash->chip->gran = WRITE_GRAN_256BYTES;
	flash->chip->block_erasers[0].eraseblocks[0].size = data->chip->total_size * KiB;
	flash->chip->block_erasers[0].eraseblocks[0].count = 1;
	return 1;
}

static int ezp_read(struct flashctx *flash, uint8_t *buf, unsigned int start, unsigned int len)
{
	struct ezp_xpro_data *data = flash->mst->opaque.data;
	if (!ezp_range_valid(data, start, len))
		return 1;
	if (!len)
		return 0;
	if (ezp_command(data, 0x0d, 0))
		return 1;
	/* The firmware streams from address zero; consume it completely even for a partial read. */
	uint8_t block[EZP_BLOCK_SIZE];
	unsigned int size = data->chip->total_size * KiB;
	for (unsigned int offset = 0; offset < size; offset += sizeof(block)) {
		if (ezp_transfer(data, EZP_READ_EP, block, sizeof(block), 5000))
			return 1;
		unsigned int first = max(offset, start);
		unsigned int last = min(offset + sizeof(block), start + len);
		if (first < last)
			memcpy(buf + first - start, block + first - offset, last - first);
	}
	return ezp_command(data, 0x09, 0);
}

static int ezp_write(struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len)
{
	struct ezp_xpro_data *data = flash->mst->opaque.data;
	if (!ezp_range_valid(data, start, len) || data->firmware != 0x03) {
		msg_perr("EZP XPro programming requires an identified chip and firmware 1.3.\n");
		return 1;
	}
	if (!len)
		return 0;
	unsigned int size = data->chip->total_size * KiB;
	uint8_t *expected = malloc(size);
	uint8_t *actual = malloc(size);
	int ret = 1;
	if (!expected || !actual)
		goto out;
	if (ezp_read(flash, expected, 0, size))
		goto failed;
	memcpy(actual, expected, size);
	memcpy(expected + start, buf, len);
	for (unsigned int offset = start / EZP_BLOCK_SIZE * EZP_BLOCK_SIZE;
	     offset < start + len; offset += EZP_BLOCK_SIZE) {
		uint8_t block[EZP_BLOCK_SIZE];
		uint8_t handshake;
		if (ezp_command(data, 0x13, offset) ||
		    ezp_transfer(data, EZP_READ_EP, block, sizeof(block), 5000) ||
		    ezp_transfer(data, EZP_READ_EP, &handshake, 1, 50000))
			goto failed;
		msg_pdbg("EZP XPro block 0x%x read handshake: 0x%02x\n", offset, handshake);
		if (memcmp(block, actual + offset, sizeof(block))) {
			msg_perr("EZP XPro sector contents changed since the initial read.\n");
			goto failed;
		}
		if (ezp_transfer(data, EZP_DATA_EP, expected + offset, EZP_BLOCK_SIZE, 60000) ||
		    ezp_command(data, 0x02, offset) ||
		    ezp_transfer(data, EZP_READ_EP, &handshake, 1, 60000))
			goto failed;
		msg_pdbg("EZP XPro block 0x%x program handshake: 0x%02x\n", offset, handshake);
	}
	if (ezp_command(data, 0x09, 0) || ezp_read(flash, actual, 0, size))
		goto failed;
	/* Firmware completion bytes are not decoded status: verify the entire array, including preserved bytes. */
	if (memcmp(expected, actual, size)) {
		msg_perr("EZP XPro whole-chip readback differs after programming.\n");
		goto failed;
	}
	ret = 0;
	goto out;
failed:
	data->failed = true;
out:
	free(actual);
	free(expected);
	return ret;
}

static int ezp_erase(struct flashctx *flash, unsigned int start, unsigned int len)
{
	struct ezp_xpro_data *data = flash->mst->opaque.data;
	unsigned int size = data->chip->total_size * KiB;
	if (!ezp_range_valid(data, start, len) || start || len != size || data->firmware != 0x03) {
		msg_perr("EZP XPro supports only whole-chip erase on firmware 1.3.\n");
		return 1;
	}
	if (ezp_command(data, 0x05, 0))
		return 1;
	for (unsigned int attempt = 0; attempt < 600; attempt++) {
		uint8_t status;
		int received = 0;
		int ret = libusb_bulk_transfer(data->handle, EZP_READ_EP, &status, 1,
					       &received, EZP_TIMEOUT);
		if (ret == LIBUSB_ERROR_TIMEOUT && !received)
			continue;
		if (ret < 0 || received != 1)
			break;
		if (status != 0x06)
			continue;
		if (ezp_command(data, 0x09, 0))
			return 1;
		uint8_t block[EZP_BLOCK_SIZE];
		if (ezp_command(data, 0x0d, 0))
			return 1;
		bool blank = true;
		for (unsigned int offset = 0; offset < size; offset += sizeof(block)) {
			if (ezp_transfer(data, EZP_READ_EP, block, sizeof(block), 5000))
				return 1;
			for (unsigned int i = 0; i < sizeof(block); i++)
				blank &= block[i] == 0xff;
		}
		if (ezp_command(data, 0x09, 0))
			return 1;
		if (blank)
			return 0;
		msg_perr("EZP XPro chip is not blank after erase.\n");
		break;
	}
	data->failed = true;
	msg_perr("EZP XPro erase did not complete and verify successfully.\n");
	return 1;
}

static const struct opaque_master ezp_master = {
	.probe = ezp_probe,
	.read = ezp_read,
	.write = ezp_write,
	.erase = ezp_erase,
	.shutdown = ezp_shutdown,
};

static int ezp_init(const struct programmer_cfg *cfg)
{
	char *chip_name = extract_programmer_param_str(cfg, "chip");
	char *voltage = extract_programmer_param_str(cfg, "voltage");
	const struct flashchip *chip = NULL;
	if (!chip_name || !*chip_name || !voltage || strcmp(voltage, "3300") ||
	    (cfg->params && *cfg->params)) {
		msg_perr("EZP XPro requires chip=<name>,voltage=3300.\n");
		goto invalid;
	}
	for (unsigned int i = 0; i < flashchips_size && flashchips[i].name; i++) {
		if (!strcmp(flashchips[i].name, chip_name)) {
			if (chip) {
				msg_perr("Ambiguous flash chip name.\n");
				goto invalid;
			}
			chip = &flashchips[i];
		}
	}
	/* Firmware selectors are qualified individually; the chip database stays in flashrom. */
	if (!chip || strcmp(chip_name, "GD25Q80(B)") || chip->bustype != BUS_SPI ||
	    chip->voltage.min > 3300 || chip->voltage.max < 3300 ||
	    chip->total_size > 16 * 1024 || chip->page_size != 256) {
		msg_perr("Unsupported EZP XPro chip profile: %s.\n", chip_name);
		goto invalid;
	}
	free(chip_name);
	free(voltage);
	struct ezp_xpro_data *data = calloc(1, sizeof(*data));
	if (!data)
		return 1;
	data->chip = chip;
	data->packet[0] = 0xa5;
	data->packet[4] = 8;
	data->packet[5] = 2;
	ezp_put_be32(data->packet + 6, chip->total_size * KiB);
	/* GD25Q80's vendor profile uses selector 324 and the 3 V voltage selector. */
	data->packet[11] = 0x03;
	data->packet[12] = 0x24;
	data->packet[19] = 0x5a;
	if (libusb_init(&data->usb) < 0) {
		free(data);
		return 1;
	}
	data->handle = libusb_open_device_with_vid_pid(data->usb, 0x0716, 0x5018);
	if (!data->handle) {
		msg_perr("Cannot open EZP XPro 0716:5018; check USB permissions.\n");
		goto fail;
	}
	if (libusb_set_configuration(data->handle, 1) < 0 ||
	    libusb_claim_interface(data->handle, 0) < 0)
		goto fail;
	data->claimed = true;
	uint8_t version;
	if (ezp_command(data, 0x12, 0) ||
	    ezp_transfer(data, EZP_READ_EP, &version, 1, EZP_TIMEOUT) || version == 0xff)
		goto fail;
	msg_pinfo("EZP XPro firmware %x.%x\n", (version >> 4) + 1, version & 0x0f);
	data->firmware = version;
	return register_opaque_master(&ezp_master, data);

fail:
	ezp_shutdown(data);
	return 1;
invalid:
	free(chip_name);
	free(voltage);
	return 1;
}

const struct programmer_entry programmer_ezp_xpro = {
	.name = "ezp_xpro",
	.type = USB,
	.devs.dev = devs_ezp_xpro,
	.init = ezp_init,
};
