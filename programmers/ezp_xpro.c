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
#include "ezp_xpro_profiles.h"

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
	const struct ezp_xpro_profile *profile;
	unsigned int block_size;
	uint8_t packet[EZP_PACKET_SIZE];
	bool claimed;
	bool active;
	bool ready;
	bool failed;
	bool erase_command;
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
			unsigned int argument, unsigned int timeout)
{
	uint8_t packet[EZP_PACKET_SIZE];
	memcpy(packet, data->packet, sizeof(packet));
	packet[1] = (packet[1] & 0xc0) | command;
	ezp_put_be32(packet + 13, argument);
	return ezp_transfer(data, EZP_COMMAND_EP, packet, sizeof(packet), timeout);
}

static int ezp_wait_completion(struct ezp_xpro_data *data, uint8_t expected,
			       unsigned int attempts, unsigned int timeout)
{
	for (unsigned int attempt = 0; attempt < attempts; attempt++) {
		uint8_t status;
		int received = 0;
		int ret = libusb_bulk_transfer(data->handle, EZP_READ_EP, &status, 1, &received, timeout);
		if (ret == LIBUSB_ERROR_TIMEOUT && !received)
			continue;
		if (ret < 0 || received != 1)
			break;
		if (status == expected)
			return 0;
	}
	data->failed = true;
	msg_perr("EZP XPro did not report completion 0x%02x.\n", expected);
	return 1;
}

static int ezp_configure(struct ezp_xpro_data *data)
{
	if (ezp_command(data, 0x00, 0, EZP_TIMEOUT))
		return 1;
	default_delay(30 * 1000);
	return 0;
}

static int ezp_shutdown(void *context)
{
	struct ezp_xpro_data *data = context;
	int ret = 0;
	if (data->active)
		ret = ezp_command(data, 0x09, 0, EZP_TIMEOUT);
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
	return data->ready && !data->failed && start <= size && len <= size - start;
}

static int ezp_probe(struct flashctx *flash)
{
	struct ezp_xpro_data *data = flash->mst->opaque.data;
	uint8_t id[3];
	data->ready = false;
	if (data->failed)
		return 0;
	bool eeprom = data->profile->family == 2;
	if (eeprom && !flash->flags.force) {
		msg_perr("EZP XPro cannot identify the selected EEPROM; --force is required to assume its identity.\n");
		return 0;
	}
	data->active = true;
	if (ezp_configure(data))
		return 0;
	if (eeprom) {
		msg_pwarn("Assuming EEPROM %s as requested by --force; the chip was not detected.\n", data->chip->name);
	} else {
		if (ezp_command(data, 0x07, 0, EZP_TIMEOUT) ||
		    ezp_transfer(data, EZP_READ_EP, id, sizeof(id), EZP_TIMEOUT))
			return 0;
		uint32_t wire_id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
		msg_pinfo("EZP XPro chip ID: %02x %02x %02x\n", id[0], id[1], id[2]);
		if (wire_id != data->profile->jedec &&
		    (!data->profile->peer_jedec || wire_id != data->profile->peer_jedec)) {
			msg_perr("Chip ID does not match selected chip %s.\n", data->chip->name);
			return 0;
		}
	}
	data->ready = true;
	flash->chip->vendor = data->chip->vendor;
	flash->chip->name = data->chip->name;
	flash->chip->total_size = data->chip->total_size;
	flash->chip->page_size = data->chip->page_size;
	flash->chip->voltage = data->chip->voltage;
	flash->chip->tested = TEST_UNTESTED;
	/* Hardware qualification is separate from firmware profile availability. */
	if (!strcmp(data->chip->name, "GD25Q80(B)"))
		flash->chip->tested = data->firmware == 0x03 ? TEST_OK_PREW : TEST_OK_PROBE;
	flash->chip->tested.wp = NA;
	flash->chip->feature_bits = eeprom ? data->chip->feature_bits & (FEATURE_NO_ERASE | FEATURE_ERASED_ZERO) : 0;
	if (eeprom && data->chip->probe == NO_PROBE_FUNC)
		flash->chip->tested.probe = NA;
	/* Batch changed bytes into pages to avoid repeated whole-image verification. */
	flash->chip->gran = eeprom ? WRITE_GRAN_1BYTE_IMPLICIT_ERASE : WRITE_GRAN_256BYTES;
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
	if (data->profile->family == 2 && ezp_configure(data))
		return 1;
	if (ezp_command(data, 0x0d, 0, EZP_TIMEOUT))
		return 1;
	/* The firmware streams from address zero; consume it completely even for a partial read. */
	uint8_t block[EZP_BLOCK_SIZE];
	unsigned int size = data->chip->total_size * KiB;
	for (unsigned int offset = 0; offset < size; offset += sizeof(block)) {
		unsigned int chunk = min((unsigned int)sizeof(block), size - offset);
		if (ezp_transfer(data, EZP_READ_EP, block, chunk, 5000))
			return 1;
		unsigned int first = max(offset, start);
		unsigned int last = min(offset + chunk, start + len);
		if (first < last)
			memcpy(buf + first - start, block + first - offset, last - first);
	}
	return ezp_command(data, 0x09, 0, EZP_TIMEOUT);
}

static int ezp_erase(struct flashctx *flash, unsigned int start, unsigned int len);

static int ezp_full_program(struct ezp_xpro_data *data, uint8_t *image, unsigned int size)
{
	if (ezp_configure(data) || ezp_command(data, 0x01, 0, 5000))
		return 1;
	default_delay(50 * 1000);
	for (unsigned int offset = 0; offset < size; offset += EZP_BLOCK_SIZE) {
		unsigned int chunk = min(EZP_BLOCK_SIZE, size - offset);
		if (ezp_transfer(data, EZP_DATA_EP, image + offset, chunk, 60000))
			return 1;
	}
	default_delay(30 * 1000);
	return ezp_command(data, 0x02, size, EZP_TIMEOUT) ||
	       ezp_wait_completion(data, 0x02, 10, 6000) ||
	       ezp_command(data, 0x09, 0, EZP_TIMEOUT);
}

static int ezp_write(struct flashctx *flash, const uint8_t *buf, unsigned int start, unsigned int len)
{
	struct ezp_xpro_data *data = flash->mst->opaque.data;
	if (!ezp_range_valid(data, start, len) || data->firmware != 0x03) {
		msg_perr("EZP XPro programming requires a prepared target and firmware 1.3.\n");
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
	if (!data->block_size) {
		bool blank = true;
		for (unsigned int i = 0; i < size; i++)
			blank &= actual[i] == 0xff;
		/* F profiles have no block replacement; preserve the entire image through erase/program. */
		if ((!blank && ezp_erase(flash, 0, size)) || ezp_full_program(data, expected, size))
			goto failed;
		goto verify;
	}
	if (data->profile->family == 2 && ezp_configure(data))
		goto failed;
	for (unsigned int offset = start / data->block_size * data->block_size;
	     offset < start + len; offset += data->block_size) {
		uint8_t block[1024];
		uint8_t handshake;
		if (ezp_command(data, 0x13, offset, EZP_TIMEOUT))
			goto failed;
		for (unsigned int pos = 0; pos < data->block_size; pos += sizeof(block)) {
			unsigned int chunk = min((unsigned int)sizeof(block), data->block_size - pos);
			if (ezp_transfer(data, EZP_READ_EP, block, chunk, 5000))
				goto failed;
			if (memcmp(block, actual + offset + pos, chunk)) {
				msg_perr("EZP XPro sector contents changed since the initial read.\n");
				goto failed;
			}
		}
		if (ezp_transfer(data, EZP_READ_EP, &handshake, 1, 50000))
			goto failed;
		msg_pdbg("EZP XPro block 0x%x read handshake: 0x%02x\n", offset, handshake);
		for (unsigned int pos = 0; pos < data->block_size; pos += EZP_BLOCK_SIZE) {
			unsigned int chunk = min(EZP_BLOCK_SIZE, data->block_size - pos);
			if (ezp_transfer(data, EZP_DATA_EP, expected + offset + pos, chunk, 60000))
				goto failed;
		}
		if (ezp_command(data, 0x02, offset, EZP_TIMEOUT) ||
		    ezp_transfer(data, EZP_READ_EP, &handshake, 1, 60000))
			goto failed;
		msg_pdbg("EZP XPro block 0x%x program handshake: 0x%02x\n", offset, handshake);
	}
	if (ezp_command(data, 0x09, 0, EZP_TIMEOUT))
		goto failed;
verify:
	if (ezp_read(flash, actual, 0, size))
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
	if (data->profile->family == 2 || !data->erase_command) {
		if (!ezp_range_valid(data, start, len) || data->firmware != 0x03)
			return 1;
		if (!len)
			return 0;
		uint8_t *blank = malloc(len);
		if (!blank)
			return 1;
		memset(blank, ERASED_VALUE(flash), len);
		int ret = ezp_write(flash, blank, start, len);
		free(blank);
		return ret;
	}
	if (!ezp_range_valid(data, start, len) || start || len != size || data->firmware != 0x03) {
		msg_perr("EZP XPro supports only whole-chip erase on firmware 1.3.\n");
		return 1;
	}
	if (ezp_command(data, 0x05, 0, EZP_TIMEOUT) ||
	    ezp_wait_completion(data, 0x06, 600, EZP_TIMEOUT) ||
	    ezp_command(data, 0x09, 0, EZP_TIMEOUT))
		return 1;
	uint8_t block[EZP_BLOCK_SIZE];
	if (ezp_command(data, 0x0d, 0, EZP_TIMEOUT))
		return 1;
	bool blank = true;
	for (unsigned int offset = 0; offset < size; offset += sizeof(block)) {
		unsigned int chunk = min((unsigned int)sizeof(block), size - offset);
		if (ezp_transfer(data, EZP_READ_EP, block, chunk, 5000))
			return 1;
		for (unsigned int i = 0; i < chunk; i++)
			blank &= block[i] == 0xff;
	}
	if (ezp_command(data, 0x09, 0, EZP_TIMEOUT))
		return 1;
	if (blank)
		return 0;

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

static bool ezp_has_eraser(const struct flashchip *chip, unsigned int block_size,
			 bool whole_chip)
{
	for (unsigned int i = 0; i < NUM_ERASEFUNCTIONS; i++) {
		const struct block_eraser *eraser = &chip->block_erasers[i];
		if (!eraser->block_erase ||
		    (whole_chip && eraser->block_erase != SPI_BLOCK_ERASE_C7 &&
		     eraser->block_erase != SPI_BLOCK_ERASE_60 &&
		     !(eraser->block_erase == SPI_BLOCK_ERASE_62 && chip->probe == PROBE_SPI_AT25F)))
			continue;
		if (eraser->eraseblocks[0].size == block_size &&
		    eraser->eraseblocks[0].count == chip->total_size * KiB / block_size &&
		    !eraser->eraseblocks[1].count)
			return true;
	}
	return false;
}

static bool ezp_identity_matches(const struct flashchip *chip, const struct ezp_xpro_profile *profile)
{
	uint32_t manufacturer = chip->manufacture_id, model = chip->model_id, wire;
	switch (chip->probe) {
	case PROBE_SPI_RDID:
		if (manufacturer <= 0xff && model <= 0xffff)
			wire = (manufacturer << 16) | model;
		else if ((manufacturer >> 8) == 0x7f && model <= 0xff) {
			/* Keep the legacy rotated candidate too, so conflicting PMC algorithms remain ambiguous. */
			wire = profile->subtype == 0x70 ?
				((manufacturer & 0xff) << 16) | (model << 8) | 0x7f :
				(manufacturer << 8) | model;
		} else
			return false;
		break;
	case PROBE_SPI_RDID4:
		/* Vendor AMIC identities omit 7F but retain both model bytes. */
		if (manufacturer != 0x7f37 || model > 0xffff)
			return false;
		wire = ((manufacturer & 0xff) << 16) | model;
		break;
	case PROBE_SPI_AT25F:
		if (manufacturer > 0xff || model > 0xff || profile->subtype != 0x20)
			return false;
		wire = (manufacturer << 8) | model;
		break;
	case PROBE_SPI_REMS:
	case PROBE_SPI_RES2:
		if (manufacturer > 0xff || model > 0xff)
			return false;
		if (profile->subtype == 0x70)
			wire = (manufacturer << 16) | (model << 8) | 0x7f;
		else if (profile->subtype == 0x90)
			wire = (manufacturer << 16) | (model << 8);
		else if (profile->subtype == 0x80 || profile->subtype == 0x60)
			wire = (manufacturer << 8) | model;
		else
			return false;
		break;
	default:
		return false;
	}
	return profile->jedec == wire;
}

static bool ezp_candidate_matches(const struct flashchip *chip, const struct ezp_xpro_profile *profile)
{
	if (profile->size != chip->total_size * KiB)
		return false;
	if (profile->family == 2)
		return !strcmp(profile->name, chip->name) && profile->page_size == chip->page_size;
	if (profile->family != 0 || !ezp_identity_matches(chip, profile))
		return false;
	if (profile->page_size == chip->page_size)
		return true;
	if (!profile->page_size)
		return chip->write == SPI_WRITE_AAI || chip->write == SPI_CHIP_WRITE1;
	/* The ESMT scalar is a firmware algorithm unit, not flashrom's physical page size. */
	return profile->subtype == 0x30 && profile->page_size == 2 && chip->write == SPI_CHIP_WRITE1 &&
	       !strcmp(profile->name, chip->name);
}

static const struct ezp_xpro_profile *ezp_profile(const struct flashchip *chip, unsigned int voltage)
{
	if (chip->bustype != BUS_SPI || chip->spi_cmd_set != SPI25 || chip->read != SPI_CHIP_READ ||
	    (chip->write != SPI_CHIP_WRITE256 && chip->write != SPI_CHIP_WRITE1 && chip->write != SPI_WRITE_AAI) ||
	    !chip->total_size || chip->total_size > 64 * 1024 ||
	    chip->voltage.min > voltage || chip->voltage.max < voltage ||
	    !chip->page_size || (chip->page_size & (chip->page_size - 1)) ||
	    (chip->feature_bits & FEATURE_STATUS_PER_DIE))
		return NULL;
	bool named = false;
	for (unsigned int i = 0; i < ARRAY_SIZE(ezp_xpro_profiles); i++) {
		const struct ezp_xpro_profile *candidate = &ezp_xpro_profiles[i];
		if (ezp_candidate_matches(chip, candidate) && !strcmp(candidate->name, chip->name))
			named = true;
	}
	const struct ezp_xpro_profile *profile = NULL;
	for (unsigned int i = 0; i < ARRAY_SIZE(ezp_xpro_profiles); i++) {
		const struct ezp_xpro_profile *candidate = &ezp_xpro_profiles[i];
		if (!ezp_candidate_matches(chip, candidate) || (named && strcmp(candidate->name, chip->name)))
			continue;
		/* Compare all remaining variants, including F, before checking supported operations. */
		if (profile && (profile->family != candidate->family || profile->page_size != candidate->page_size ||
				profile->block_size != candidate->block_size || profile->selector != candidate->selector ||
				profile->subtype != candidate->subtype)) {
			msg_perr("Ambiguous EZP XPro firmware profiles for %s.\n", chip->name);
			return NULL;
		}
		if (!profile || !candidate->alternate)
			profile = candidate;
	}
	if (!profile)
		return NULL;
	if (profile->family == 2) {
		unsigned int selector = chip->feature_bits & FEATURE_ADDR_2BYTE ? 0x216 : 0x324;
		if (profile->subtype || profile->block_size || profile->selector != selector ||
		    (chip->probe != NO_PROBE_FUNC && chip->probe != PROBE_SPI_ST95) ||
		    chip->write != SPI_CHIP_WRITE256 || !(chip->feature_bits & FEATURE_NO_ERASE) ||
		    chip->block_erasers[0].block_erase != SPI_BLOCK_ERASE_EMULATION ||
		    !ezp_has_eraser(chip, chip->total_size * KiB, false))
			return NULL;
		return profile;
	}
	if (voltage != 3300 || (chip->feature_bits & (FEATURE_NO_ERASE | FEATURE_ERASED_ZERO | FEATURE_ADDR_2BYTE)))
		return NULL;
	/* These are opaque firmware selectors, not host-issued SPI mode commands. */
	bool addressing_supported =
		(profile->selector == 0x324 && chip->total_size <= 16 * 1024) ||
		(profile->selector == 0x4b7 && chip->total_size > 16 * 1024 &&
		 profile->subtype == 0 && chip->page_size == 256 && profile->block_size == 4 * KiB &&
		 (chip->feature_bits & (FEATURE_4BA_ENTER | FEATURE_4BA_ENTER_WREN | FEATURE_4BA_ENTER_EAR7)));
	bool algorithm_supported =
		(chip->write == SPI_CHIP_WRITE256 &&
		 (profile->subtype == 0 || profile->subtype == 0x10 || profile->subtype == 0x20 ||
		  profile->subtype == 0x50 || profile->subtype == 0x60 || profile->subtype == 0x70)) ||
		(chip->write == SPI_WRITE_AAI && profile->subtype == 0x90 && !profile->page_size) ||
		(chip->write == SPI_CHIP_WRITE1 && (profile->subtype == 0x30 || profile->subtype == 0x80));
	if (!addressing_supported || !algorithm_supported)
		return NULL;
	if (!profile->block_size)
		return profile->page_size && (profile->subtype == 0 || profile->subtype == 0x10) &&
		       ezp_has_eraser(chip, chip->total_size * KiB, true) ? profile : NULL;
	if (profile->block_size < EZP_BLOCK_SIZE || profile->block_size > 256 * KiB ||
	    (profile->block_size & (profile->block_size - 1)) ||
	    chip->total_size * KiB % profile->block_size || !ezp_has_eraser(chip, profile->block_size, false))
		return NULL;
	return profile;
}

static int ezp_init(const struct programmer_cfg *cfg)
{
	char *chip_name = extract_programmer_param_str(cfg, "chip");
	char *voltage = extract_programmer_param_str(cfg, "voltage");
	const struct flashchip *chip = NULL;
	if (!chip_name || !*chip_name || !voltage || (strcmp(voltage, "3300") && strcmp(voltage, "5000")) ||
	    (cfg->params && *cfg->params)) {
		msg_perr("EZP XPro requires chip=<name>,voltage=3300 or voltage=5000.\n");
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
	unsigned int voltage_mv = voltage && !strcmp(voltage, "5000") ? 5000 : 3300;
	const struct ezp_xpro_profile *profile = chip ? ezp_profile(chip, voltage_mv) : NULL;
	if (!profile) {
		msg_perr("Unsupported EZP XPro chip profile: %s.\n", chip_name);
		goto invalid;
	}
	free(chip_name);
	free(voltage);
	struct ezp_xpro_data *data = calloc(1, sizeof(*data));
	if (!data)
		return 1;
	data->chip = chip;
	data->profile = profile;
	data->block_size = profile->family == 2 ? profile->page_size : profile->block_size;
	data->erase_command = profile->family == 0 && ezp_has_eraser(chip, chip->total_size * KiB, true);
	data->packet[0] = 0xa5;
	data->packet[1] = voltage_mv == 5000 ? 0x80 : 0;
	data->packet[2] = profile->family;
	data->packet[3] = profile->subtype;
	if (!profile->page_size)
		data->packet[4] = 0xff;
	else
		for (unsigned int size = profile->page_size; size > 1; size >>= 1)
			data->packet[4]++;
	if (!profile->block_size && profile->family == 0)
		data->packet[5] = 0xff;
	else
		for (unsigned int size = data->block_size / KiB; size > 1; size >>= 1)
			data->packet[5]++;
	ezp_put_be32(data->packet + 6, chip->total_size * KiB);
	data->packet[11] = profile->selector >> 8;
	data->packet[12] = profile->selector;
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
	if (ezp_command(data, 0x12, 0, EZP_TIMEOUT) ||
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
