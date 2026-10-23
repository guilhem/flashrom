/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "lifecycle.h"

#if CONFIG_EZP_XPRO == 1
#include <limits.h>
#include <libusb.h>

#define EZP_PARAMS "chip=GD25Q80(B),voltage=3300"
#define EZP_CHIP_SIZE (1024 * KiB)
#define EZP_READ_BLOCK 4096
#define EZP_READ_TRANSFERS (5 + 1 + EZP_CHIP_SIZE / EZP_READ_BLOCK + 2)

struct ezp_example {
	const char *name;
	unsigned int size, page, block;
	uint8_t id[3];
	uint8_t packet[20];
};

/* Independent vendor v2.8 rows and complete frames, not the generated driver table. */
static const struct ezp_example ezp_examples[] = {
	/* @GD25Q80:256:4KB:1MB:N:324:C84014:N:3:000 */
	{ "GD25Q80(B)", 1 * MiB, 256, 4 * KiB, { 0xc8, 0x40, 0x14 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 0x10, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @GD25Q64:256:4KB:8MB:N:324:C84017:N:3:000 */
	{ "GD25Q64(B)", 8 * MiB, 256, 4 * KiB, { 0xc8, 0x40, 0x17 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 0x80, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @W25Q64:256:4KB:8MB:N:324:EF4017:N:3:000 */
	{ "W25Q64BV/W25Q64CV/W25Q64FV", 8 * MiB, 256, 4 * KiB, { 0xef, 0x40, 0x17 },
	  { 0xa5, 0, 0, 0, 8, 2, 0, 0x80, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @W25Q128:256:4KB:16MB:N:324:EF4018:N:3:000 */
	{ "W25Q128.V", 16 * MiB, 256, 4 * KiB, { 0xef, 0x40, 0x18 },
	  { 0xa5, 0, 0, 0, 8, 2, 1, 0, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @MX25L5121E:32:4KB:64KB:N:324:C22210:N:3:000 */
	{ "MX25L5121E", 64 * KiB, 32, 4 * KiB, { 0xc2, 0x22, 0x10 },
	  { 0xa5, 0, 0, 0, 5, 2, 0, 1, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @M25P10A:256:32KB:128KB:N:324:202011:101010:3:010 */
	{ "M25P10-A", 128 * KiB, 256, 32 * KiB, { 0x20, 0x20, 0x11 },
	  { 0xa5, 0, 0, 0x10, 8, 5, 0, 2, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @S25FL016A:256:64KB:2MB:N:324:010214:N:3:010 */
	{ "S25FL016A", 2 * MiB, 256, 64 * KiB, { 1, 2, 0x14 },
	  { 0xa5, 0, 0, 0x10, 8, 6, 0, 0x20, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @M25P128:256:256KB:16MB:N:324:202018:171717:3:010 */
	{ "M25P128", 16 * MiB, 256, 256 * KiB, { 0x20, 0x20, 0x18 },
	  { 0xa5, 0, 0, 0x10, 8, 8, 1, 0, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
	/* @F25L32PA:256:4KB:4MB:N:324:8C2016:N:3:050 */
	{ "F25L32PA", 4 * MiB, 256, 4 * KiB, { 0x8c, 0x20, 0x16 },
	  { 0xa5, 0, 0, 0x50, 8, 2, 0, 0x40, 0, 0, 0, 3, 0x24, 0, 0, 0, 0, 0, 0, 0x5a } },
};

struct ezp_transfer {
	uint8_t endpoint;
	uint8_t command;
	int length;
	uint8_t response[3];
	int error;
	int missing;
	unsigned int offset;
};

struct ezp_io_state {
	const struct ezp_transfer *transfers;
	size_t count;
	size_t next;
	unsigned int usb_inits;
};

static const struct ezp_transfer ezp_probe_transfers[] = {
	{ .endpoint = 0x02, .command = 0x12, .length = 20 },
	{ .endpoint = 0x83, .length = 1, .response = { 0x10 } },
	{ .endpoint = 0x02, .command = 0x00, .length = 20 },
	{ .endpoint = 0x02, .command = 0x07, .length = 20 },
	{ .endpoint = 0x83, .length = 3, .response = { 0xc8, 0x40, 0x14 } },
	{ .endpoint = 0x02, .command = 0x09, .length = 20 },
};

static int ezp_libusb_init(void *state, libusb_context **ctx)
{
	struct ezp_io_state *s = state;
	s->usb_inits++;
	*ctx = not_null();
	return 0;
}

static uint8_t ezp_read_byte(unsigned int offset)
{
	return (offset ^ (offset >> 8) ^ (offset >> 16)) * 37 + 11;
}

static void ezp_check_example_packet(const struct ezp_example *example,
		const uint8_t *data, uint8_t command, unsigned int address)
{
	uint8_t packet[20];
	memcpy(packet, example->packet, sizeof(packet));
	packet[1] = command;
	for (unsigned int i = 0; i < 4; i++)
		packet[13 + i] = address >> (24 - i * 8);
	assert_memory_equal(data, packet, sizeof(packet));
}

static void ezp_check_packet(const uint8_t *data, uint8_t command, unsigned int address)
{
	ezp_check_example_packet(&ezp_examples[0], data, command, address);
}

static int ezp_libusb_bulk_transfer(void *state, libusb_device_handle *devh,
		unsigned char endpoint, unsigned char *data, int length,
		int *actual_length, unsigned int timeout)
{
	struct ezp_io_state *s = state;
	assert_true(s->next < s->count);
	const struct ezp_transfer *t = &s->transfers[s->next++];
	assert_ptr_equal(devh, not_null());
	assert_int_equal(endpoint, t->endpoint);
	assert_int_equal(length, t->length);
	assert_true(timeout > 0);
	if (endpoint == 0x02) {
		ezp_check_packet(data, t->command, 0);
	} else if (length == EZP_READ_BLOCK) {
		for (int i = 0; i < length - t->missing; i++)
			data[i] = ezp_read_byte(t->offset + i);
	} else {
		assert_true((size_t)length <= sizeof(t->response));
		memcpy(data, t->response, length - t->missing);
	}
	*actual_length = length - t->missing;
	return t->error;
}

static struct io_mock ezp_io(struct ezp_io_state *state)
{
	return (struct io_mock) {
		.state = state,
		.libusb_init = ezp_libusb_init,
		.libusb_bulk_transfer = ezp_libusb_bulk_transfer,
	};
}

void ezp_xpro_invalid_params_test(void **state)
{
	const char *params[] = {
		NULL, "", "chip=GD25Q80(B)", "voltage=3300",
		"chip=,voltage=3300", "chip=GD25Q80(B),voltage=",
		"chip=GD25Q80(B),voltage=1800", "chip=GD25Q80(B),voltage=3.3V",
		"chip=GD25Q80(B),voltage=3300x",
		"chip=not-a-flash-chip,voltage=3300",
		"chip=GD25Q80,voltage=3300",
		"chip=GD25LQ80,voltage=3300",
		EZP_PARAMS ",unexpected=yes",
	};
	for (size_t i = 0; i < ARRAY_SIZE(params); i++) {
		struct ezp_io_state s = { 0 };
		const struct io_mock io = ezp_io(&s);
		run_init_error_path(state, &io, &programmer_ezp_xpro, params[i], 1);
		assert_int_equal(s.usb_inits, 0);
		assert_int_equal(registered_master_count, 0);
	}
}

void ezp_xpro_profile_guards_test(void **state)
{
	const char *names[] = {
		"GD25LQ80", /* 1.8 V. */
		"F49B002UA", /* Parallel bus. */
		"unknown Macronix SPI chip", /* Generic identity. */
		"M25P10", /* RES probe and byte writes, not the vendor's 128-byte RDID row. */
		"AT25F512A", /* Special probe and subtype 020. */
		"SST25VF016B", /* AAI writes. */
		"AT45DB041D", /* DataFlash command set. */
		"W25Q256FV", /* More than 16 MiB. */
		"AT25DF021", /* Vendor F sector geometry. */
		"S25FL032A/P", "S25FL064A/P", /* Same ID, conflicting A/P geometry and subtype. */
		"AT25F512B", /* Vendor page 128 disagrees with flashrom page 256. */
	};
	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		bool exists = false;
		for (unsigned int j = 0; j < flashchips_size && flashchips[j].name; j++)
			exists |= !strcmp(names[i], flashchips[j].name);
		assert_true(exists);
		char params[128];
		snprintf(params, sizeof(params), "chip=%s,voltage=3300", names[i]);
		struct ezp_io_state s = { 0 };
		const struct io_mock io = ezp_io(&s);
		run_init_error_path(state, &io, &programmer_ezp_xpro, params, 1);
		assert_int_equal(s.usb_inits, 0);
		assert_int_equal(registered_master_count, 0);
	}
}

struct ezp_inventory {
	unsigned int transfers, usb_inits;
};

static int ezp_inventory_init(void *state, libusb_context **ctx)
{
	struct ezp_inventory *s = state;
	s->usb_inits++;
	*ctx = not_null();
	return 0;
}

static int ezp_inventory_transfer(void *state, libusb_device_handle *devh,
		unsigned char endpoint, unsigned char *data, int length,
		int *actual_length, unsigned int timeout)
{
	struct ezp_inventory *s = state;
	assert_ptr_equal(devh, not_null());
	assert_int_equal(timeout, 1000);
	if (s->transfers++ == 0) {
		assert_int_equal(endpoint, 0x02);
		assert_int_equal(length, 20);
		assert_int_equal(data[0], 0xa5);
		assert_int_equal(data[1], 0x12);
		assert_int_equal(data[19], 0x5a);
	} else {
		assert_int_equal(s->transfers, 2);
		assert_int_equal(endpoint, 0x83);
		assert_int_equal(length, 1);
		data[0] = 0x03;
	}
	*actual_length = length;
	return 0;
}

void ezp_xpro_profile_inventory_test(void **state)
{
	(void)state;
	unsigned int accepted = 0, rejected = 0, duplicate_names = 0;
	for (unsigned int i = 0; i < flashchips_size && flashchips[i].name; i++) {
		bool duplicate = false;
		for (unsigned int j = 0; j < i; j++)
			duplicate |= !strcmp(flashchips[i].name, flashchips[j].name);
		if (duplicate) {
			duplicate_names++;
			continue;
		}
		char params[256];
		int len = snprintf(params, sizeof(params), "chip=%s,voltage=3300", flashchips[i].name);
		assert_true(len > 0 && (size_t)len < sizeof(params));
		struct ezp_inventory s = { 0 };
		const struct io_mock io = {
			.state = &s, .libusb_init = ezp_inventory_init,
			.libusb_bulk_transfer = ezp_inventory_transfer,
		};
		io_mock_register(&io);
		int ret = programmer_init(&programmer_ezp_xpro, params);
		if (!ret) {
			accepted++;
			printf("EZP accepted flashrom profile: %s\n", flashchips[i].name);
			assert_int_equal(s.usb_inits, 1);
			assert_int_equal(s.transfers, 2);
			assert_int_equal(registered_master_count, 1);
		} else {
			rejected++;
			assert_int_equal(s.usb_inits, 0);
			assert_int_equal(s.transfers, 0);
			assert_int_equal(registered_master_count, 0);
		}
		assert_int_equal(programmer_shutdown(), 0);
		assert_int_equal(registered_master_count, 0);
		io_mock_register(NULL);
	}
	assert_true(accepted >= ARRAY_SIZE(ezp_examples));
	assert_true(rejected > 0);
	printf("EZP mocked runtime inventory: %u accepted, %u rejected unique names, %u duplicate rows skipped; no hardware.\n",
	       accepted, rejected, duplicate_names);
}

void ezp_xpro_ambiguous_params_test(void **state)
{
	const char *params[] = {
		EZP_PARAMS ",chip=GD25Q80(B)",
		EZP_PARAMS ",chip=GD25LQ80",
		EZP_PARAMS ",voltage=3300",
		EZP_PARAMS ",voltage=1800",
	};
	for (size_t i = 0; i < ARRAY_SIZE(params); i++) {
		struct ezp_io_state s = { 0 };
		const struct io_mock io = ezp_io(&s);
		run_init_error_path(state, &io, &programmer_ezp_xpro, params[i], 1);
		assert_int_equal(s.usb_inits, 0);
	}
}

void ezp_xpro_basic_lifecycle_test_success(void **state)
{
	struct ezp_io_state s = { .transfers = ezp_probe_transfers, .count = 2 };
	const struct io_mock io = ezp_io(&s);
	run_basic_lifecycle(state, &io, &programmer_ezp_xpro, EZP_PARAMS);
	assert_int_equal(s.usb_inits, 1);
	assert_int_equal(s.next, s.count);
}

void ezp_xpro_master_registration_failure_test(void **state)
{
	struct registered_master master = { 0 };
	int ret;
	assert_int_equal(registered_master_count, 0);
	while ((ret = register_master(&master)) == 0)
		;
	assert_int_equal(ret, ERROR_FLASHROM_LIMIT);
	assert_true(registered_master_count > 0);
	struct ezp_io_state s = { .transfers = ezp_probe_transfers, .count = 2 };
	const struct io_mock io = ezp_io(&s);
	/* The framework retains cleanup ownership when the master table is full. */
	run_init_error_path(state, &io, &programmer_ezp_xpro, EZP_PARAMS, ERROR_FLASHROM_LIMIT);
	assert_int_equal(s.next, s.count);
	assert_int_equal(registered_master_count, 0);
	assert_int_equal(programmer_shutdown(), 0);
}

static int ezp_count_shutdown(void *data)
{
	unsigned int *count = data;
	(*count)++;
	return 0;
}

void ezp_xpro_shutdown_registration_failure_test(void **state)
{
	(void)state;
	struct ezp_io_state s = { .transfers = ezp_probe_transfers, .count = 2 };
	const struct io_mock io = ezp_io(&s);
	io_mock_register(&io);
	/* A rejected init enables shutdown registration without opening USB. */
	assert_int_equal(programmer_init(&programmer_ezp_xpro, NULL), 1);
	assert_int_equal(s.usb_inits, 0);
	unsigned int slots = 0, shutdowns = 0;
	while (register_shutdown(ezp_count_shutdown, &shutdowns) == 0)
		slots++;
	assert_true(slots > 0);
	/* With no shutdown slot, the framework cleans up immediately instead. */
	assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 1);
	assert_int_equal(registered_master_count, 0);
	assert_int_equal(s.next, s.count);
	assert_int_equal(programmer_shutdown(), 0);
	assert_int_equal(shutdowns, slots);
	assert_int_equal(programmer_shutdown(), 0);
	assert_int_equal(shutdowns, slots);
	io_mock_register(NULL);
}

void ezp_xpro_firmware_transfer_error_test(void **state)
{
	for (size_t failed = 0; failed < 2; failed++) {
		for (int short_transfer = 0; short_transfer < 2; short_transfer++) {
			struct ezp_transfer transfers[2];
			memcpy(transfers, ezp_probe_transfers, sizeof(transfers));
			transfers[failed].error = short_transfer ? 0 : LIBUSB_ERROR_IO;
			transfers[failed].missing = short_transfer ? 1 : 0;
			struct ezp_io_state s = { .transfers = transfers, .count = failed + 1 };
			const struct io_mock io = ezp_io(&s);
			run_init_error_path(state, &io, &programmer_ezp_xpro, EZP_PARAMS, 1);
			assert_int_equal(s.next, s.count);
			assert_int_equal(registered_master_count, 0);
		}
	}
}

void ezp_xpro_firmware_invalid_test(void **state)
{
	struct ezp_transfer transfers[2];
	memcpy(transfers, ezp_probe_transfers, sizeof(transfers));
	transfers[1].response[0] = 0xff;
	struct ezp_io_state s = { .transfers = transfers, .count = ARRAY_SIZE(transfers) };
	const struct io_mock io = ezp_io(&s);
	run_init_error_path(state, &io, &programmer_ezp_xpro, EZP_PARAMS, 1);
	assert_int_equal(s.next, s.count);
	assert_int_equal(registered_master_count, 0);
}

void ezp_xpro_probe_lifecycle_test_success(void **state)
{
	struct ezp_io_state s = {
		.transfers = ezp_probe_transfers, .count = ARRAY_SIZE(ezp_probe_transfers),
	};
	const struct io_mock io = ezp_io(&s);
	const char *names[] = { "GD25Q80(B)" };
	run_probe_v2_lifecycle(state, &io, &programmer_ezp_xpro, EZP_PARAMS,
			       NULL, names, ARRAY_SIZE(names));
	assert_int_equal(s.next, s.count);
}

static void ezp_probe_rejected(struct ezp_transfer *transfers, size_t count)
{
	struct ezp_io_state s = { .transfers = transfers, .count = count };
	const struct io_mock io = ezp_io(&s);
	io_mock_register(&io);
	assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 0);
	assert_int_equal(registered_master_count, 1);
	struct flashchip chip = { 0 };
	struct flashctx flash = { .chip = &chip, .mst = &registered_masters[0] };
	const struct opaque_master *master = &flash.mst->opaque;
	assert_int_equal(master->probe(&flash), 0);
	uint8_t byte = 0;
	size_t after_probe = s.next;
	assert_int_not_equal(master->read(&flash, &byte, 0, 1), 0);
	assert_int_not_equal(master->write(&flash, &byte, 0, 1), 0);
	assert_int_not_equal(master->erase(&flash, 0, 1024 * KiB), 0);
	assert_int_equal(s.next, after_probe);
	assert_int_equal(programmer_shutdown(), 0);
	assert_int_equal(s.next, s.count);
	io_mock_register(NULL);
}

void ezp_xpro_probe_mismatch_test(void **state)
{
	(void)state;
	const uint8_t ids[][3] = {
		{ 0xef, 0x40, 0x14 }, { 0xc8, 0x60, 0x14 }, { 0xc8, 0x40, 0x15 },
		{ 0xff, 0xff, 0xff }, { 0x00, 0x00, 0x00 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
		struct ezp_transfer transfers[ARRAY_SIZE(ezp_probe_transfers)];
		memcpy(transfers, ezp_probe_transfers, sizeof(transfers));
		memcpy(transfers[4].response, ids[i], sizeof(ids[i]));
		ezp_probe_rejected(transfers, ARRAY_SIZE(transfers));
	}
}

void ezp_xpro_probe_transfer_error_test(void **state)
{
	(void)state;
	for (size_t failed = 2; failed <= 4; failed++) {
		for (int short_transfer = 0; short_transfer < 2; short_transfer++) {
			struct ezp_transfer transfers[ARRAY_SIZE(ezp_probe_transfers)];
			memcpy(transfers, ezp_probe_transfers, sizeof(transfers));
			transfers[failed].error = short_transfer ? 0 : LIBUSB_ERROR_IO;
			transfers[failed].missing = short_transfer ? 1 : 0;
			transfers[failed + 1] = ezp_probe_transfers[5];
			ezp_probe_rejected(transfers, failed + 2);
		}
	}
}

static size_t ezp_read_transfers(struct ezp_transfer *transfers)
{
	memcpy(transfers, ezp_probe_transfers, 5 * sizeof(*transfers));
	size_t n = 5;
	transfers[n++] = (struct ezp_transfer) { .endpoint = 0x02, .command = 0x0d, .length = 20 };
	for (unsigned int offset = 0; offset < EZP_CHIP_SIZE; offset += EZP_READ_BLOCK)
		transfers[n++] = (struct ezp_transfer) {
			.endpoint = 0x83, .length = EZP_READ_BLOCK, .offset = offset,
		};
	transfers[n++] = ezp_probe_transfers[5];
	transfers[n++] = ezp_probe_transfers[5];
	return n;
}

static void ezp_init_probe(struct flashctx *flash)
{
	assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 0);
	assert_int_equal(registered_master_count, 1);
	flash->mst = &registered_masters[0];
	assert_int_equal(flash->mst->opaque.probe(flash), 1);
	assert_string_equal(flash->chip->name, "GD25Q80(B)");
	assert_int_equal(flash->chip->total_size * KiB, EZP_CHIP_SIZE);
	assert_int_equal(flash->chip->page_size, 256);
	assert_int_equal(flash->chip->gran, WRITE_GRAN_256BYTES);
	assert_int_equal(flash->chip->block_erasers[0].eraseblocks[0].size, EZP_CHIP_SIZE);
	assert_int_equal(flash->chip->block_erasers[0].eraseblocks[0].count, 1);
}

void ezp_xpro_shutdown_transfer_error_test(void **state)
{
	(void)state;
	for (int short_transfer = 0; short_transfer < 2; short_transfer++) {
		struct ezp_transfer transfers[ARRAY_SIZE(ezp_probe_transfers)];
		memcpy(transfers, ezp_probe_transfers, sizeof(transfers));
		transfers[5].error = short_transfer ? 0 : LIBUSB_ERROR_IO;
		transfers[5].missing = short_transfer ? 1 : 0;
		struct ezp_io_state s = { .transfers = transfers, .count = ARRAY_SIZE(transfers) };
		const struct io_mock io = ezp_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_probe(&flash);
		assert_int_not_equal(programmer_shutdown(), 0);
		assert_int_equal(s.next, s.count);
		assert_int_equal(registered_master_count, 0);
		io_mock_register(NULL);
	}
}

void ezp_xpro_reprobe_mismatch_test(void **state)
{
	(void)state;
	struct ezp_transfer transfers[9];
	memcpy(transfers, ezp_probe_transfers, 5 * sizeof(*transfers));
	memcpy(transfers + 5, ezp_probe_transfers + 2, 3 * sizeof(*transfers));
	memset(transfers[7].response, 0xff, sizeof(transfers[7].response));
	transfers[8] = ezp_probe_transfers[5];
	struct ezp_io_state s = { .transfers = transfers, .count = ARRAY_SIZE(transfers) };
	const struct io_mock io = ezp_io(&s);
	io_mock_register(&io);
	struct flashchip chip = { 0 };
	struct flashctx flash = { .chip = &chip };
	ezp_init_probe(&flash);
	assert_int_equal(flash.mst->opaque.probe(&flash), 0);
	uint8_t byte = 0;
	assert_int_not_equal(flash.mst->opaque.read(&flash, &byte, 0, 1), 0);
	assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 0, 1), 0);
	assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, EZP_CHIP_SIZE), 0);
	assert_int_equal(s.next, 8);
	assert_int_equal(programmer_shutdown(), 0);
	assert_int_equal(s.next, s.count);
	io_mock_register(NULL);
}

void ezp_xpro_read_ranges_test(void **state)
{
	(void)state;
	const unsigned int ranges[][2] = {
		{ 0, 1 }, { EZP_CHIP_SIZE - 1, 1 }, { EZP_READ_BLOCK - 1, 3 },
		{ EZP_READ_BLOCK, EZP_READ_BLOCK }, { 17, 8211 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(ranges); i++) {
		struct ezp_transfer transfers[EZP_READ_TRANSFERS];
		struct ezp_io_state s = { .transfers = transfers, .count = ezp_read_transfers(transfers) };
		const struct io_mock io = ezp_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_probe(&flash);
		unsigned int start = ranges[i][0], len = ranges[i][1];
		uint8_t *buf = malloc(len + 2);
		assert_non_null(buf);
		memset(buf, 0xa7, len + 2);
		assert_int_equal(flash.mst->opaque.read(&flash, buf + 1, start, len), 0);
		assert_int_equal(buf[0], 0xa7);
		assert_int_equal(buf[len + 1], 0xa7);
		for (unsigned int j = 0; j < len; j++)
			assert_int_equal(buf[j + 1], ezp_read_byte(start + j));
		free(buf);
		assert_int_equal(programmer_shutdown(), 0);
		assert_int_equal(s.next, s.count);
		io_mock_register(NULL);
	}
}

void ezp_xpro_read_image_test(void **state)
{
	(void)state;
	struct ezp_transfer transfers[EZP_READ_TRANSFERS];
	struct ezp_io_state s = { .transfers = transfers, .count = ezp_read_transfers(transfers) };
	const struct io_mock io = ezp_io(&s);
	io_mock_register(&io);
	struct flashrom_programmer *prog = NULL;
	struct flashctx *flash = NULL;
	const char **names = NULL;
	assert_int_equal(flashrom_programmer_init(&prog, programmer_ezp_xpro.name, EZP_PARAMS), 0);
	assert_int_equal(flashrom_create_context(&flash), 0);
	assert_int_equal(flashrom_flash_probe_v2(flash, &names, prog, NULL), 1);
	assert_string_equal(names[0], "GD25Q80(B)");
	flashrom_data_free(names);
	uint8_t *buf = malloc(EZP_CHIP_SIZE);
	assert_non_null(buf);
	assert_int_equal(flashrom_image_read(flash, buf, EZP_CHIP_SIZE), 0);
	for (unsigned int i = 0; i < EZP_CHIP_SIZE; i++)
		assert_int_equal(buf[i], ezp_read_byte(i));
	free(buf);
	flashrom_flash_release(flash);
	assert_int_equal(flashrom_programmer_shutdown(prog), 0);
	assert_int_equal(s.next, s.count);
	io_mock_register(NULL);
}

void ezp_xpro_read_invalid_range_test(void **state)
{
	(void)state;
	struct ezp_io_state s = {
		.transfers = ezp_probe_transfers, .count = ARRAY_SIZE(ezp_probe_transfers),
	};
	const struct io_mock io = ezp_io(&s);
	io_mock_register(&io);
	struct flashchip chip = { 0 };
	struct flashctx flash = { .chip = &chip };
	assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 0);
	assert_int_equal(registered_master_count, 1);
	flash.mst = &registered_masters[0];
	uint8_t byte = 0xa7;
	assert_int_not_equal(flash.mst->opaque.read(&flash, &byte, 0, 1), 0);
	assert_int_equal(s.next, 2);
	assert_int_equal(flash.mst->opaque.probe(&flash), 1);
	const unsigned int ranges[][2] = {
		{ EZP_CHIP_SIZE, 1 }, { EZP_CHIP_SIZE - 1, 2 }, { EZP_CHIP_SIZE + 1, 0 },
		{ UINT_MAX, 1 }, { 1, UINT_MAX },
	};
	for (size_t i = 0; i < ARRAY_SIZE(ranges); i++)
		assert_int_not_equal(flash.mst->opaque.read(&flash, &byte, ranges[i][0], ranges[i][1]), 0);
	assert_int_equal(flash.mst->opaque.read(&flash, &byte, 0, 0), 0);
	assert_int_equal(flash.mst->opaque.read(&flash, &byte, EZP_CHIP_SIZE, 0), 0);
	assert_int_equal(byte, 0xa7);
	assert_int_equal(s.next, 5);
	assert_int_equal(programmer_shutdown(), 0);
	assert_int_equal(s.next, s.count);
	io_mock_register(NULL);
}

void ezp_xpro_read_transfer_error_test(void **state)
{
	(void)state;
	const size_t failures[] = { 5, 6, 6 + 128, 6 + 255, EZP_READ_TRANSFERS - 2 };
	for (size_t i = 0; i < ARRAY_SIZE(failures); i++) {
		for (int short_transfer = 0; short_transfer < 2; short_transfer++) {
			struct ezp_transfer transfers[EZP_READ_TRANSFERS];
			ezp_read_transfers(transfers);
			size_t failed = failures[i];
			transfers[failed].error = short_transfer ? 0 : LIBUSB_ERROR_TIMEOUT;
			transfers[failed].missing = short_transfer ? 1 : 0;
			transfers[failed + 1] = ezp_probe_transfers[5];
			struct ezp_io_state s = { .transfers = transfers, .count = failed + 2 };
			const struct io_mock io = ezp_io(&s);
			io_mock_register(&io);
			struct flashchip chip = { 0 };
			struct flashctx flash = { .chip = &chip };
			ezp_init_probe(&flash);
			uint8_t buf[3] = { 0xa7, 0xa7, 0xa7 };
			assert_int_not_equal(flash.mst->opaque.read(&flash, buf + 1, 0, 1), 0);
			assert_int_equal(buf[0], 0xa7);
			assert_int_equal(buf[2], 0xa7);
			assert_int_equal(programmer_shutdown(), 0);
			assert_int_equal(s.next, s.count);
			io_mock_register(NULL);
		}
	}
}

enum ezp_memory_phase {
	EZP_IDLE, EZP_VERSION, EZP_ID, EZP_STREAM, EZP_SECTOR, EZP_READ_ACK,
	EZP_PAYLOAD, EZP_COMMIT, EZP_PROGRAM_ACK, EZP_ERASE_POLL,
};

struct ezp_memory {
	const struct ezp_example *example;
	uint8_t *contents;
	uint8_t *pending;
	enum ezp_memory_phase phase;
	uint8_t firmware, read_ack, program_ack;
	unsigned int address, stream_offset, sector_offset, payload_offset;
	unsigned int streams, calls, programs, sectors, payloads, erases, polls;
	int fail_command;
	enum ezp_memory_phase fail_phase;
	unsigned int fail_stream;
	unsigned int skip_fail_matches;
	bool short_transfer, faulted;
	int transfer_error;
	unsigned int corrupt_at;
	bool corrupt_sector, erase_nonblank, erase_stuck;
	unsigned int erase_timeouts, erase_busy;
};

static struct ezp_memory ezp_memory_new_example(const struct ezp_example *example)
{
	struct ezp_memory s = {
		.example = example,
		.firmware = 0x03, .read_ack = 0xa5, .program_ack = 0xff,
		.fail_command = -1, .corrupt_at = UINT_MAX, .transfer_error = LIBUSB_ERROR_IO,
	};
	s.contents = malloc(example->size);
	s.pending = malloc(example->block);
	assert_non_null(s.contents);
	assert_non_null(s.pending);
	for (unsigned int i = 0; i < example->size; i++)
		s.contents[i] = ezp_read_byte(i);
	return s;
}

static struct ezp_memory ezp_memory_new(void)
{
	return ezp_memory_new_example(&ezp_examples[0]);
}

static int ezp_memory_transfer(void *state, libusb_device_handle *devh,
		unsigned char endpoint, unsigned char *data, int length,
		int *actual_length, unsigned int timeout)
{
	struct ezp_memory *s = state;
	const struct ezp_example *example = s->example;
	s->calls++;
	assert_ptr_equal(devh, not_null());
	*actual_length = length;
	bool command = endpoint == 0x02;
	if (command) {
		assert_int_equal(length, 20);
		assert_int_equal(timeout, 1000);
	} else if (endpoint == 0x01) {
		assert_int_equal(length, EZP_READ_BLOCK);
		assert_int_equal(timeout, 60000);
	} else {
		assert_int_equal(endpoint, 0x83);
		unsigned int expected_timeout = 1000;
		int expected_length = 1;
		switch (s->phase) {
		case EZP_ID: expected_length = 3; break;
		case EZP_STREAM: expected_length = EZP_READ_BLOCK; expected_timeout = 5000; break;
		case EZP_SECTOR: expected_length = 1024; expected_timeout = 5000; break;
		case EZP_READ_ACK: expected_timeout = 50000; break;
		case EZP_PROGRAM_ACK: expected_timeout = 60000; break;
		default: break;
		}
		assert_int_equal(length, expected_length);
		assert_int_equal(timeout, expected_timeout);
	}
	bool fault_matches = (command && data[1] == s->fail_command) ||
	    (!command && s->fail_phase != EZP_IDLE && s->phase == s->fail_phase &&
	     (!s->fail_stream || s->streams == s->fail_stream));
	if (fault_matches && s->skip_fail_matches) {
		s->skip_fail_matches--;
		fault_matches = false;
	}
	if (!s->faulted && fault_matches) {
		s->faulted = true;
		*actual_length = s->short_transfer ? length - 1 : 0;
		return s->short_transfer ? 0 : s->transfer_error;
	}
	if (command) {
		assert_int_equal(length, 20);
		unsigned int address = 0;
		for (unsigned int i = 13; i < 17; i++)
			address = (address << 8) | data[i];
		ezp_check_example_packet(example, data, data[1], address);
		if (data[1] != 0x13 && data[1] != 0x02)
			assert_int_equal(address, 0);
		switch (data[1]) {
		case 0x12:
			assert_int_equal(s->phase, EZP_IDLE);
			s->phase = EZP_VERSION;
			break;
		case 0x00:
			assert_int_equal(s->phase, EZP_IDLE);
			break;
		case 0x07:
			assert_int_equal(s->phase, EZP_IDLE);
			s->phase = EZP_ID;
			break;
		case 0x0d:
			assert_int_equal(s->phase, EZP_IDLE);
			s->phase = EZP_STREAM;
			s->stream_offset = 0;
			s->streams++;
			break;
		case 0x13:
			assert_int_equal(s->phase, EZP_IDLE);
			assert_int_equal(address % example->block, 0);
			assert_true(address <= example->size - example->block);
			s->address = address;
			s->sector_offset = 0;
			s->payload_offset = 0;
			s->phase = EZP_SECTOR;
			break;
		case 0x02:
			assert_int_equal(s->phase, EZP_COMMIT);
			assert_int_equal(address, s->address);
			/* The simulated device applies only the received payload, independently of the requested range. */
			memcpy(s->contents + address, s->pending, example->block);
			if (s->corrupt_at < example->size)
				s->contents[s->corrupt_at] ^= 1;
			s->programs++;
			s->phase = EZP_PROGRAM_ACK;
			break;
		case 0x05:
			assert_int_equal(s->phase, EZP_IDLE);
			s->erases++;
			s->phase = EZP_ERASE_POLL;
			break;
		case 0x09:
			if (!s->faulted && s->phase == EZP_STREAM)
				assert_int_equal(s->stream_offset, example->size);
			s->phase = EZP_IDLE;
			break;
		default:
			fail_msg("Unexpected EZP command 0x%02x", data[1]);
		}
		return 0;
	}
	if (endpoint == 0x01) {
		assert_int_equal(s->phase, EZP_PAYLOAD);
		assert_int_equal(length, EZP_READ_BLOCK);
		assert_true(s->payload_offset <= example->block - length);
		memcpy(s->pending + s->payload_offset, data, length);
		s->payload_offset += length;
		s->payloads++;
		if (s->payload_offset == example->block)
			s->phase = EZP_COMMIT;
		return 0;
	}
	assert_int_equal(endpoint, 0x83);
	switch (s->phase) {
	case EZP_VERSION:
		assert_int_equal(length, 1);
		data[0] = s->firmware;
		s->phase = EZP_IDLE;
		break;
	case EZP_ID: {
		assert_int_equal(length, sizeof(example->id));
		memcpy(data, example->id, sizeof(example->id));
		s->phase = EZP_IDLE;
		break;
	}
	case EZP_STREAM:
		assert_int_equal(length, EZP_READ_BLOCK);
		assert_true(s->stream_offset <= example->size - EZP_READ_BLOCK);
		memcpy(data, s->contents + s->stream_offset, length);
		s->stream_offset += length;
		break;
	case EZP_SECTOR:
		assert_int_equal(length, 1024);
		assert_true(s->sector_offset <= example->block - length);
		memcpy(data, s->contents + s->address + s->sector_offset, length);
		if (s->corrupt_sector)
			data[0] ^= 1;
		s->sector_offset += length;
		s->sectors++;
		if (s->sector_offset == example->block)
			s->phase = EZP_READ_ACK;
		break;
	case EZP_READ_ACK:
		assert_int_equal(length, 1);
		data[0] = s->read_ack;
		s->phase = EZP_PAYLOAD;
		break;
	case EZP_PROGRAM_ACK:
		assert_int_equal(length, 1);
		data[0] = s->program_ack;
		s->phase = EZP_IDLE;
		break;
	case EZP_ERASE_POLL:
		assert_int_equal(length, 1);
		assert_int_equal(timeout, 1000);
		assert_true(++s->polls <= 600);
		if (s->erase_timeouts) {
			s->erase_timeouts--;
			*actual_length = 0;
			return LIBUSB_ERROR_TIMEOUT;
		}
		if (s->erase_stuck || s->erase_busy) {
			if (s->erase_busy)
				s->erase_busy--;
			data[0] = 0xa5;
			break;
		}
		data[0] = 0x06;
		memset(s->contents, 0xff, example->size);
		if (s->erase_nonblank)
			s->contents[example->size - 1] = 0;
		s->phase = EZP_IDLE;
		break;
	default:
		fail_msg("Unexpected EZP receive phase %d", s->phase);
	}
	return 0;
}

static struct io_mock ezp_memory_io(struct ezp_memory *s)
{
	return (struct io_mock) { .state = s, .libusb_bulk_transfer = ezp_memory_transfer };
}

static void ezp_assert_locked(struct flashctx *flash, const struct ezp_memory *s)
{
	unsigned int calls = s->calls;
	uint8_t byte = 0;
	assert_int_not_equal(flash->mst->opaque.read(flash, &byte, 0, 1), 0);
	assert_int_not_equal(flash->mst->opaque.write(flash, &byte, 0, 1), 0);
	assert_int_not_equal(flash->mst->opaque.erase(flash, 0, s->example->size), 0);
	assert_int_equal(flash->mst->opaque.probe(flash), 0);
	assert_int_equal(s->calls, calls);
}

static void ezp_memory_finish(struct ezp_memory *s)
{
	assert_int_equal(programmer_shutdown(), 0);
	io_mock_register(NULL);
	free(s->contents);
	free(s->pending);
}

static void ezp_init_example(struct flashctx *flash, const struct ezp_example *example)
{
	char params[128];
	snprintf(params, sizeof(params), "chip=%s,voltage=3300", example->name);
	assert_int_equal(programmer_init(&programmer_ezp_xpro, params), 0);
	assert_int_equal(registered_master_count, 1);
	flash->mst = &registered_masters[0];
	assert_int_equal(flash->mst->opaque.probe(flash), 1);
	assert_string_equal(flash->chip->name, example->name);
	assert_int_equal(flash->chip->total_size * KiB, example->size);
	assert_int_equal(flash->chip->page_size, example->page);
	assert_int_equal(flash->chip->block_erasers[0].eraseblocks[0].size, example->size);
	assert_int_equal(flash->chip->block_erasers[0].eraseblocks[0].count, 1);
}

void ezp_xpro_profile_status_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < 2 * ARRAY_SIZE(ezp_examples); i++) {
		struct ezp_memory s = ezp_memory_new_example(&ezp_examples[i / 2]);
		s.firmware = i % 2 ? 0x04 : 0x03;
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_example(&flash, s.example);
		ezp_memory_finish(&s);
		assert_int_equal(chip.tested.probe, i / 2 == 0 ? OK : NT);
		assert_int_equal(chip.tested.read, i == 0 ? OK : NT);
		assert_int_equal(chip.tested.erase, i == 0 ? OK : NT);
		assert_int_equal(chip.tested.write, i == 0 ? OK : NT);
		assert_int_equal(chip.tested.wp, NA);
	}
}

void ezp_xpro_profile_transfer_error_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < ARRAY_SIZE(ezp_examples); i++) {
		for (unsigned int phase = 0; phase < 2; phase++) {
			for (unsigned int error = 0; error < 3; error++) {
				struct ezp_memory s = ezp_memory_new_example(&ezp_examples[i]);
				s.fail_phase = phase ? EZP_PAYLOAD : EZP_SECTOR;
				s.skip_fail_matches = s.example->block / (phase ? 4096 : 1024) - 1;
				s.short_transfer = error == 0;
				s.transfer_error = error == 1 ? LIBUSB_ERROR_TIMEOUT : LIBUSB_ERROR_IO;
				const struct io_mock io = ezp_memory_io(&s);
				io_mock_register(&io);
				struct flashchip chip = { 0 };
				struct flashctx flash = { .chip = &chip };
				ezp_init_example(&flash, s.example);
				uint8_t *before = malloc(s.example->size);
				assert_non_null(before);
				memcpy(before, s.contents, s.example->size);
				uint8_t byte = 0x59;
				assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, s.example->size - 1, 1), 0);
				assert_true(s.faulted);
				assert_int_equal(s.programs, 0);
				assert_int_equal(s.sectors, s.example->block / 1024 - (phase ? 0 : 1));
				assert_int_equal(s.payloads, phase ? s.example->block / 4096 - 1 : 0);
				assert_memory_equal(before, s.contents, s.example->size);
				free(before);
				ezp_assert_locked(&flash, &s);
				ezp_memory_finish(&s);
			}
		}
	}
}

void ezp_xpro_profile_write_ranges_test(void **state)
{
	(void)state;
	for (size_t i = 0; i < ARRAY_SIZE(ezp_examples); i++) {
		struct ezp_memory s = ezp_memory_new_example(&ezp_examples[i]);
		const struct ezp_example *example = s.example;
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_example(&flash, example);
		const unsigned int ranges[][2] = {
			{ example->page - 1, 3 },
			{ example->block - 7, 2 * example->block + 19 },
			{ example->size - 1, 1 },
		};
		uint8_t *expected = malloc(example->size);
		uint8_t *buf = malloc(2 * example->block + 19);
		assert_non_null(expected);
		assert_non_null(buf);
		memcpy(expected, s.contents, example->size);
		unsigned int programs = 0;
		for (size_t j = 0; j < ARRAY_SIZE(ranges); j++) {
			unsigned int start = ranges[j][0], len = ranges[j][1];
			for (unsigned int k = 0; k < len; k++)
				buf[k] = 0xff ^ (k * 23 + j);
			memcpy(expected + start, buf, len);
			assert_int_equal(flash.mst->opaque.write(&flash, buf, start, len), 0);
			/* Check the entire chip so preserved bytes and later payload chunks cannot be lost. */
			assert_memory_equal(s.contents, expected, example->size);
			programs += (start + len - 1) / example->block - start / example->block + 1;
			assert_int_equal(s.programs, programs);
			assert_int_equal(s.sectors, programs * example->block / 1024);
			assert_int_equal(s.payloads, programs * example->block / EZP_READ_BLOCK);
			assert_int_equal(s.streams, 2 * (j + 1));
		}
		free(buf);
		free(expected);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_write_ranges_test(void **state)
{
	(void)state;
	const unsigned int ranges[][2] = {
		{ 0, 1 }, { EZP_CHIP_SIZE - 1, 1 }, { 255, 3 },
		{ EZP_READ_BLOCK - 1, 3 }, { EZP_READ_BLOCK - 7, 2 * EZP_READ_BLOCK + 19 },
		{ 123, 1 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(ranges); i++) {
		struct ezp_memory s = ezp_memory_new();
		s.read_ack = i;
		s.program_ack = 0xff - i;
		if (i == ARRAY_SIZE(ranges) - 1)
			memset(s.contents, 0, EZP_CHIP_SIZE);
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_probe(&flash);
		unsigned int start = ranges[i][0], len = ranges[i][1];
		uint8_t *expected = malloc(EZP_CHIP_SIZE), *buf = malloc(len);
		assert_non_null(expected);
		assert_non_null(buf);
		memcpy(expected, s.contents, EZP_CHIP_SIZE);
		for (unsigned int j = 0; j < len; j++)
			buf[j] = 0xff ^ (j * 23);
		memcpy(expected + start, buf, len);
		assert_int_equal(flash.mst->opaque.write(&flash, buf, start, len), 0);
		assert_memory_equal(s.contents, expected, EZP_CHIP_SIZE);
		assert_int_equal(s.streams, 2);
		assert_int_equal(s.programs, (start + len - 1) / EZP_READ_BLOCK - start / EZP_READ_BLOCK + 1);
		assert_int_equal(s.payloads, s.programs);
		free(buf);
		free(expected);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_image_write_verify_erase_test(void **state)
{
	(void)state;
	struct ezp_memory s = ezp_memory_new();
	memset(s.contents, 0xff, EZP_CHIP_SIZE);
	const struct io_mock io = ezp_memory_io(&s);
	io_mock_register(&io);
	struct flashctx *flash = NULL;
	const char **names = NULL;
	assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 0);
	assert_int_equal(flashrom_create_context(&flash), 0);
	assert_int_equal(flashrom_flash_probe_v2(flash, &names, NULL, NULL), 1);
	flashrom_data_free(names);
	assert_int_equal(flash->chip->gran, WRITE_GRAN_256BYTES);
	uint8_t *image = malloc(EZP_CHIP_SIZE);
	assert_non_null(image);
	memcpy(image, s.contents, EZP_CHIP_SIZE);
	image[17] = 0xa5;
	image[19] = 0x5a;
	image[23] = 0;
	image[2 * 256 + 9] = 0x34;
	image[EZP_READ_BLOCK + 5] = 0x12;
	image[EZP_CHIP_SIZE - 17] = 0x55;
	flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_AFTER_WRITE, true);
	flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_WHOLE_CHIP, true);
	assert_int_equal(flashrom_image_write(flash, image, EZP_CHIP_SIZE, NULL), 0);
	assert_memory_equal(s.contents, image, EZP_CHIP_SIZE);
	assert_int_equal(s.programs, 4);
	assert_int_equal(s.erases, 0);
	assert_int_equal(flashrom_image_verify(flash, image, EZP_CHIP_SIZE), 0);
	s.contents[EZP_CHIP_SIZE - 1] ^= 1;
	assert_int_not_equal(flashrom_image_verify(flash, image, EZP_CHIP_SIZE), 0);
	assert_int_equal(flashrom_flash_erase(flash), 0);
	memset(image, 0xff, EZP_CHIP_SIZE);
	assert_memory_equal(s.contents, image, EZP_CHIP_SIZE);
	assert_int_equal(s.erases, 1);
	free(image);
	flashrom_flash_release(flash);
	ezp_memory_finish(&s);
}

void ezp_xpro_partial_layout_auto_erase_test(void **state)
{
	(void)state;
	struct ezp_memory s = ezp_memory_new();
	const unsigned int start = EZP_READ_BLOCK - 1, len = 3;
	s.contents[start] = 0;
	const struct io_mock io = ezp_memory_io(&s);
	io_mock_register(&io);
	struct flashctx *flash = NULL;
	const char **names = NULL;
	assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 0);
	assert_int_equal(flashrom_create_context(&flash), 0);
	assert_int_equal(flashrom_flash_probe_v2(flash, &names, NULL, NULL), 1);
	flashrom_data_free(names);
	struct flashrom_layout *layout = NULL;
	assert_int_equal(flashrom_layout_new(&layout), 0);
	assert_int_equal(flashrom_layout_add_region(layout, start, start + len - 1, "update"), 0);
	assert_int_equal(flashrom_layout_include_region(layout, "update"), 0);
	flashrom_layout_set(flash, layout);
	flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_AFTER_WRITE, true);
	flashrom_flag_set(flash, FLASHROM_FLAG_VERIFY_WHOLE_CHIP, true);
	uint8_t *image = malloc(EZP_CHIP_SIZE), *expected = malloc(EZP_CHIP_SIZE);
	assert_non_null(image);
	assert_non_null(expected);
	memcpy(expected, s.contents, EZP_CHIP_SIZE);
	/* Excluded image bytes deliberately disagree with the chip; only the selected range may change. */
	memset(image, 0x96, EZP_CHIP_SIZE);
	memset(image + start, 0xff, len);
	memset(expected + start, 0xff, len);
	assert_int_equal(flashrom_image_write(flash, image, EZP_CHIP_SIZE, NULL), 0);
	assert_int_equal(s.erases, 1);
	assert_true(s.programs > 0);
	assert_memory_equal(s.contents, expected, EZP_CHIP_SIZE);
	free(expected);
	free(image);
	flashrom_layout_set(flash, NULL);
	flashrom_layout_release(layout);
	flashrom_flash_release(flash);
	ezp_memory_finish(&s);
}

void ezp_xpro_destructive_guards_test(void **state)
{
	(void)state;
	const uint8_t firmware[] = { 0x03, 0x00, 0x02, 0x04, 0x10, 0xfe };
	for (size_t i = 0; i < ARRAY_SIZE(firmware); i++) {
		struct ezp_memory s = ezp_memory_new();
		s.firmware = firmware[i];
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		assert_int_equal(programmer_init(&programmer_ezp_xpro, EZP_PARAMS), 0);
		assert_int_equal(registered_master_count, 1);
		flash.mst = &registered_masters[0];
		uint8_t byte = 0;
		unsigned int calls = s.calls;
		assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 0, 1), 0);
		assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, EZP_CHIP_SIZE), 0);
		assert_int_equal(s.calls, calls);
		assert_int_equal(flash.mst->opaque.probe(&flash), 1);
		calls = s.calls;
		const unsigned int ranges[][2] = {
			{ EZP_CHIP_SIZE, 1 }, { EZP_CHIP_SIZE - 1, 2 }, { EZP_CHIP_SIZE + 1, 0 },
			{ UINT_MAX, 1 }, { 1, UINT_MAX },
		};
		for (size_t j = 0; j < ARRAY_SIZE(ranges); j++)
			assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, ranges[j][0], ranges[j][1]), 0);
		assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, EZP_READ_BLOCK), 0);
		assert_int_not_equal(flash.mst->opaque.erase(&flash, 1, EZP_CHIP_SIZE - 1), 0);
		assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, 0), 0);
		if (s.firmware == 0x03) {
			assert_int_equal(flash.mst->opaque.write(&flash, &byte, 0, 0), 0);
			assert_int_equal(flash.mst->opaque.write(&flash, &byte, EZP_CHIP_SIZE, 0), 0);
		} else {
			assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 0, 1), 0);
			assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, EZP_CHIP_SIZE), 0);
		}
		assert_int_equal(s.calls, calls);
		assert_int_equal(flash.mst->opaque.read(&flash, &byte, 0, 1), 0);
		assert_int_equal(byte, ezp_read_byte(0));
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_write_transfer_error_test(void **state)
{
	(void)state;
	const struct { int command; enum ezp_memory_phase phase; unsigned int stream, skip; } failures[] = {
		{ 0x0d, EZP_IDLE, 0, 0 }, { -1, EZP_STREAM, 1, 0 }, { 0x09, EZP_IDLE, 0, 0 },
		{ 0x13, EZP_IDLE, 0, 0 }, { -1, EZP_SECTOR, 0, 0 }, { -1, EZP_READ_ACK, 0, 0 },
		{ -1, EZP_PAYLOAD, 0, 0 }, { 0x02, EZP_IDLE, 0, 0 }, { -1, EZP_PROGRAM_ACK, 0, 0 },
		{ -1, EZP_STREAM, 2, 0 }, { 0x0d, EZP_IDLE, 0, 1 },
		{ 0x09, EZP_IDLE, 0, 1 }, { 0x09, EZP_IDLE, 0, 2 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(failures); i++) {
		for (int short_transfer = 0; short_transfer < 2; short_transfer++) {
			struct ezp_memory s = ezp_memory_new();
			s.fail_command = failures[i].command;
			s.fail_phase = failures[i].phase;
			s.fail_stream = failures[i].stream;
			s.skip_fail_matches = failures[i].skip;
			s.short_transfer = short_transfer;
			const struct io_mock io = ezp_memory_io(&s);
			io_mock_register(&io);
			struct flashchip chip = { 0 };
			struct flashctx flash = { .chip = &chip };
			ezp_init_probe(&flash);
			uint8_t byte = 0x59;
			assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 123, 1), 0);
			assert_true(s.faulted);
			ezp_assert_locked(&flash, &s);
			ezp_memory_finish(&s);
		}
	}
}

void ezp_xpro_write_corruption_test(void **state)
{
	(void)state;
	const unsigned int corruptions[] = { UINT_MAX, 123, 124, EZP_CHIP_SIZE - 1 };
	for (size_t i = 0; i < ARRAY_SIZE(corruptions); i++) {
		struct ezp_memory s = ezp_memory_new();
		s.corrupt_sector = i == 0;
		s.corrupt_at = corruptions[i];
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_probe(&flash);
		uint8_t byte = 0x59;
		assert_int_not_equal(flash.mst->opaque.write(&flash, &byte, 123, 1), 0);
		assert_int_equal(s.payloads, i == 0 ? 0 : 1);
		assert_int_equal(s.programs, i == 0 ? 0 : 1);
		assert_int_equal(s.streams, i == 0 ? 1 : 2);
		ezp_assert_locked(&flash, &s);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_erase_polling_test(void **state)
{
	(void)state;
	struct ezp_memory s = ezp_memory_new();
	s.erase_timeouts = 2;
	s.erase_busy = 2;
	const struct io_mock io = ezp_memory_io(&s);
	io_mock_register(&io);
	struct flashchip chip = { 0 };
	struct flashctx flash = { .chip = &chip };
	ezp_init_probe(&flash);
	assert_int_equal(flash.mst->opaque.erase(&flash, 0, EZP_CHIP_SIZE), 0);
	assert_int_equal(s.polls, 5);
	assert_int_equal(s.streams, 1);
	for (unsigned int i = 0; i < EZP_CHIP_SIZE; i++)
		assert_int_equal(s.contents[i], 0xff);
	ezp_memory_finish(&s);
}

void ezp_xpro_erase_failure_test(void **state)
{
	(void)state;
	for (unsigned int mode = 0; mode < 3; mode++) {
		struct ezp_memory s = ezp_memory_new();
		s.erase_stuck = mode == 0;
		s.erase_timeouts = mode == 1 ? 600 : 0;
		s.erase_nonblank = mode == 2;
		const struct io_mock io = ezp_memory_io(&s);
		io_mock_register(&io);
		struct flashchip chip = { 0 };
		struct flashctx flash = { .chip = &chip };
		ezp_init_probe(&flash);
		assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, EZP_CHIP_SIZE), 0);
		assert_int_equal(s.polls, mode == 2 ? 1 : 600);
		assert_int_equal(s.streams, mode == 2 ? 1 : 0);
		if (mode == 2)
			assert_int_equal(s.stream_offset, EZP_CHIP_SIZE);
		ezp_assert_locked(&flash, &s);
		ezp_memory_finish(&s);
	}
}

void ezp_xpro_erase_transfer_error_test(void **state)
{
	(void)state;
	const struct { int command; enum ezp_memory_phase phase; unsigned int skip; } failures[] = {
		{ 0x05, EZP_IDLE, 0 }, { -1, EZP_ERASE_POLL, 0 }, { 0x09, EZP_IDLE, 0 },
		{ 0x0d, EZP_IDLE, 0 }, { -1, EZP_STREAM, 0 }, { 0x09, EZP_IDLE, 1 },
	};
	for (size_t i = 0; i < ARRAY_SIZE(failures); i++) {
		for (int short_transfer = 0; short_transfer < 2; short_transfer++) {
			struct ezp_memory s = ezp_memory_new();
			s.fail_command = failures[i].command;
			s.fail_phase = failures[i].phase;
			s.skip_fail_matches = failures[i].skip;
			s.short_transfer = short_transfer;
			const struct io_mock io = ezp_memory_io(&s);
			io_mock_register(&io);
			struct flashchip chip = { 0 };
			struct flashctx flash = { .chip = &chip };
			ezp_init_probe(&flash);
			assert_int_not_equal(flash.mst->opaque.erase(&flash, 0, EZP_CHIP_SIZE), 0);
			assert_true(s.faulted);
			ezp_assert_locked(&flash, &s);
			ezp_memory_finish(&s);
		}
	}
}

#else
SKIP_TEST(ezp_xpro_invalid_params_test)
SKIP_TEST(ezp_xpro_profile_guards_test)
SKIP_TEST(ezp_xpro_profile_inventory_test)
SKIP_TEST(ezp_xpro_profile_status_test)
SKIP_TEST(ezp_xpro_profile_write_ranges_test)
SKIP_TEST(ezp_xpro_profile_transfer_error_test)
SKIP_TEST(ezp_xpro_ambiguous_params_test)
SKIP_TEST(ezp_xpro_basic_lifecycle_test_success)
SKIP_TEST(ezp_xpro_master_registration_failure_test)
SKIP_TEST(ezp_xpro_shutdown_registration_failure_test)
SKIP_TEST(ezp_xpro_firmware_transfer_error_test)
SKIP_TEST(ezp_xpro_firmware_invalid_test)
SKIP_TEST(ezp_xpro_probe_lifecycle_test_success)
SKIP_TEST(ezp_xpro_probe_mismatch_test)
SKIP_TEST(ezp_xpro_probe_transfer_error_test)
SKIP_TEST(ezp_xpro_shutdown_transfer_error_test)
SKIP_TEST(ezp_xpro_reprobe_mismatch_test)
SKIP_TEST(ezp_xpro_read_ranges_test)
SKIP_TEST(ezp_xpro_read_image_test)
SKIP_TEST(ezp_xpro_read_invalid_range_test)
SKIP_TEST(ezp_xpro_read_transfer_error_test)
SKIP_TEST(ezp_xpro_write_ranges_test)
SKIP_TEST(ezp_xpro_image_write_verify_erase_test)
SKIP_TEST(ezp_xpro_partial_layout_auto_erase_test)
SKIP_TEST(ezp_xpro_destructive_guards_test)
SKIP_TEST(ezp_xpro_write_transfer_error_test)
SKIP_TEST(ezp_xpro_write_corruption_test)
SKIP_TEST(ezp_xpro_erase_polling_test)
SKIP_TEST(ezp_xpro_erase_failure_test)
SKIP_TEST(ezp_xpro_erase_transfer_error_test)
#endif
